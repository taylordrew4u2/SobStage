#include "CardCopier.h"

#include "../Platform/NativeCopyFileSystem.h"

#include <algorithm>
#include <thread>

#if defined(__APPLE__)
 #include <sys/resource.h>
#elif defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace mma {

namespace {

using Clock = std::chrono::steady_clock;

bool sameAttempt (const std::optional<cardcopy::Result>& last, const cardcopy::Result& now)
{
    return last.has_value() && last->outcome == now.outcome && last->fileName == now.fileName;
}

} // namespace

CardCopier::CardCopier (FileSystemFactory makeFileSystem)
    : state (std::make_shared<State>())
{
    state->makeFileSystem = makeFileSystem
        ? std::move (makeFileSystem)
        : FileSystemFactory ([] { return std::make_unique<NativeCopyFileSystem>(); });
}

CardCopier::~CardCopier()
{
    cancel();
    waitUntilIdle (kShutdownWaitMs);
}

bool CardCopier::enqueueLocked (State& s, TakeId id, bool atFront)
{
    if (std::find (s.queued.begin(), s.queued.end(), id) == s.queued.end())
    {
        if (atFront)
            s.queued.push_front (id);
        else
            s.queued.push_back (id);
    }

    auto& record = s.records[id];
    record.phase = Phase::Waiting;
    record.done = record.total = 0;

    if (s.running)
        return false;

    s.running = true;
    return true;
}

CardCopier::TakeId CardCopier::addTakeLocked (State& s, Job job)
{
    const auto id = s.nextId++;
    auto& record = s.records[id];
    record.awaitingFollowUps = job.moreFilesComing;
    record.job = std::move (job);
    s.newestAt[record.job.localFolder] = id;
    return id;
}

CardCopier::Record* CardCopier::newestLocked (State& s, const std::string& localFolder, TakeId* id)
{
    const auto at = s.newestAt.find (localFolder);
    if (at == s.newestAt.end())
        return nullptr;

    const auto found = s.records.find (at->second);
    if (found == s.records.end())
        return nullptr;

    if (id != nullptr)
        *id = at->second;

    return &found->second;
}

void CardCopier::startWorkerIfNeeded (bool needed)
{
    if (! needed)
        return;

    // Detached, holding its own share of the state: it may be stuck inside a
    // call to a card that went away, and nothing here may wait on that.
    try
    {
        std::thread ([s = state] { run (s); }).detach();
    }
    catch (...)
    {
        // Left queued, and tried again by the next start or retry.
        const std::lock_guard<std::mutex> lock (state->lock);
        state->running = false;
        state->changed.notify_all();
    }
}

bool CardCopier::start (Job job)
{
    bool needed = false;

    {
        const std::lock_guard<std::mutex> lock (state->lock);

        if (state->cancelling.load (std::memory_order_acquire))
            return false;

        // A record of its own, even at a folder an earlier take had: that one
        // keeps its own job, which follows its folder wherever it went.
        needed = enqueueLocked (*state, addTakeLocked (*state, std::move (job)));
    }

    startWorkerIfNeeded (needed);
    return true;
}

void CardCopier::setHeld (bool held)
{
    if (state->held.load (std::memory_order_acquire) == held)
        return;

    {
        // Under the lock the worker waits under, so a release is never missed
        // between its check and its wait.
        const std::lock_guard<std::mutex> lock (state->lock);
        state->held.store (held, std::memory_order_release);

        if (held)
            state->heldSince = Clock::now();
    }

    state->changed.notify_all();
}

std::vector<CardCopier::Finished> CardCopier::collectFinished()
{
    const std::lock_guard<std::mutex> lock (state->lock);
    std::vector<Finished> out;
    out.swap (state->finished);
    return out;
}

bool CardCopier::isRunning() const
{
    const std::lock_guard<std::mutex> lock (state->lock);
    return state->inFlight != 0 || ! state->queued.empty() || ! state->scanRoot.empty();
}

bool CardCopier::isCopying() const
{
    const std::lock_guard<std::mutex> lock (state->lock);
    return state->inFlight != 0 && ! state->held.load (std::memory_order_acquire);
}

bool CardCopier::hasWorkToFinish() const
{
    const std::lock_guard<std::mutex> lock (state->lock);

    if (state->inFlight != 0 || ! state->scanRoot.empty())
        return true;

    for (const auto id : state->queued)
    {
        const auto found = state->records.find (id);

        // A retry of a card that was out, full or locked is not work a quit
        // cuts short: the marker keeps it for the next launch either way, and
        // asking would hold every quit while the card stays out.
        if (found == state->records.end() || ! found->second.last.has_value()
            || ! cardcopy::isFailure (found->second.last->outcome))
            return true;
    }

    return false;
}

double CardCopier::msHeld() const
{
    const std::lock_guard<std::mutex> lock (state->lock);

    if (! state->held.load (std::memory_order_acquire))
        return 0.0;

    return std::chrono::duration<double, std::milli> (Clock::now() - state->heldSince).count();
}

double CardCopier::msSinceProgress() const
{
    const std::lock_guard<std::mutex> lock (state->lock);

    if (state->inFlight == 0)
        return 0.0;

    return std::chrono::duration<double, std::milli> (Clock::now() - state->lastProgress).count();
}

CardCopier::View CardCopier::view (const std::string& localFolder) const
{
    const std::lock_guard<std::mutex> lock (state->lock);
    View v;

    const auto* found = newestLocked (*state, localFolder);
    if (found == nullptr)
        return v;

    const auto& r = *found;
    v.phase = r.phase;
    v.done = r.done;
    v.total = r.total;
    v.folderNow = r.folderNow.empty() ? localFolder : r.folderNow;
    if (r.last.has_value())
        v.result = *r.last;
    return v;
}

int CardCopier::retry (const std::function<bool (const cardcopy::Result&)>& shouldRetry)
{
    int queuedNow = 0;
    bool needed = false;

    {
        const std::lock_guard<std::mutex> lock (state->lock);

        if (state->cancelling.load (std::memory_order_acquire))
            return 0;

        for (auto& [id, record] : state->records)
        {
            if (record.phase != Phase::StillOnComputer || ! record.last.has_value()
                || ! shouldRetry (*record.last))
                continue;

            needed = enqueueLocked (*state, id) || needed;
            ++queuedNow;
        }
    }

    startWorkerIfNeeded (needed);
    return queuedNow;
}

bool CardCopier::copyAgain (const std::string& localFolder)
{
    bool needed = false;

    {
        const std::lock_guard<std::mutex> lock (state->lock);

        if (state->cancelling.load (std::memory_order_acquire))
            return false;

        TakeId id = 0;
        const auto* found = newestLocked (*state, localFolder, &id);
        if (found == nullptr || found->phase == Phase::Waiting
            || found->phase == Phase::Copying || found->job.marker.destinationRoot.empty())
            return false;

        needed = enqueueLocked (*state, id);
    }

    startWorkerIfNeeded (needed);
    return true;
}

int CardCopier::followUpsFinished()
{
    int queuedNow = 0;
    bool needed = false;

    {
        const std::lock_guard<std::mutex> lock (state->lock);

        if (state->cancelling.load (std::memory_order_acquire))
            return 0;

        for (auto& [id, record] : state->records)
        {
            // Only the takes something was being made for. Every other copied
            // take is left alone: its folder may even have been deleted since,
            // as the guide suggests once the card's copy is checked.
            if (! record.awaitingFollowUps)
                continue;

            record.awaitingFollowUps = false;

            // Under way or waiting: it may already have listed its folder
            // before the new file arrived, so it goes once more afterwards,
            // and lets go of the marker then.
            if (record.phase == Phase::Copying || record.phase == Phase::Waiting)
            {
                record.againWhenDone = true;
                continue;
            }

            // A failed copy already kept its marker and is retried as before;
            // the retry carries the new file with it.
            if (record.phase != Phase::Copied)
                continue;

            record.resync = true;
            needed = enqueueLocked (*state, id) || needed;
            ++queuedNow;
        }
    }

    startWorkerIfNeeded (needed);
    return queuedNow;
}

void CardCopier::resumePendingIn (const std::string& stagingRoot)
{
    bool needed = false;

    {
        const std::lock_guard<std::mutex> lock (state->lock);

        if (state->cancelling.load (std::memory_order_acquire) || stagingRoot.empty())
            return;

        state->scanRoot = stagingRoot;

        if (! state->running)
        {
            state->running = true;
            needed = true;
        }
    }

    startWorkerIfNeeded (needed);
}

std::optional<int> CardCopier::takeResumedCount()
{
    const std::lock_guard<std::mutex> lock (state->lock);
    auto count = state->resumedCount;
    state->resumedCount.reset();
    return count;
}

CardCopier::Summary CardCopier::summary() const
{
    const std::lock_guard<std::mutex> lock (state->lock);
    Summary s;

    for (const auto& [id, record] : state->records)
    {
        if (id == state->inFlight)
        {
            s.copyingFolder = record.folderNow.empty() ? record.job.localFolder : record.folderNow;
            s.done = record.done;
            s.total = record.total;
        }
        else if (record.phase == Phase::Waiting)
        {
            ++s.waiting;
        }
        else if (record.phase == Phase::StillOnComputer && record.last.has_value()
                 && record.last->outcome != cardcopy::Outcome::ComputerCopyMissing)
        {
            if (record.last->addedFilesOnly)
            {
                ++s.addedFilesWaiting;
                continue;
            }

            ++s.onlyOnComputer;

            if (record.last->outcome == cardcopy::Outcome::CardMissing)
                ++s.cardMissing;
        }
    }

    return s;
}

void CardCopier::cancel() noexcept
{
    state->cancelling.store (true, std::memory_order_release);
    state->changed.notify_all();
}

bool CardCopier::waitUntilIdle (int timeoutMs)
{
    std::unique_lock<std::mutex> lock (state->lock);
    return state->changed.wait_for (lock, std::chrono::milliseconds (std::max (0, timeoutMs)),
                                    [this] { return ! state->running; });
}

void CardCopier::scan (State& s, cardcopy::CopyFileSystem& fs, const std::string& root)
{
    std::vector<cardcopy::Entry> entries;
    std::vector<std::pair<std::string, cardcopy::Marker>> found;

    if (fs.list (root, entries).ok())
    {
        for (const auto& e : entries)
        {
            if (! e.isFolder || e.name.empty() || e.name.front() == '.')
                continue;

            const auto folder = cardcopy::joinPath (root, e.name);
            std::string text;

            if (! fs.readText (cardcopy::joinPath (folder, cardcopy::kMarkerFileName), text).ok())
                continue;

            if (auto marker = cardcopy::markerFromJson (text))
                found.emplace_back (folder, std::move (*marker));
        }
    }

    // Take folders are named "YYYY-MM-DD_HHMM_<name>", so by name is oldest
    // first: the order they were recorded in.
    std::sort (found.begin(), found.end(),
               [] (const auto& a, const auto& b) { return a.first < b.first; });

    const std::lock_guard<std::mutex> lock (s.lock);
    int added = 0;

    for (auto& [folder, marker] : found)
    {
        // This run already knows it: started at a Stop, or found before.
        if (s.newestAt.count (folder) > 0)
            continue;

        enqueueLocked (s, addTakeLocked (s, { folder, std::move (marker), nullptr }));
        ++added;
    }

    s.resumedCount = added;
}

void CardCopier::run (std::shared_ptr<State> s)
{
   #if defined(__APPLE__)
    // The lowest disk priority, for this thread only, as the podcast copy
    // has: with nothing else using the disks it still runs at full speed.
    (void) setiopolicy_np (IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD, IOPOL_THROTTLE);
   #elif defined(_WIN32)
    // Background mode lowers this thread's disk and memory priority as well
    // as its CPU priority.
    (void) SetThreadPriority (GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
   #endif

    auto fs = s->makeFileSystem();

    for (;;)
    {
        TakeId key = 0;
        Job job;
        std::string scanRoot;
        bool keepMarker = false;
        std::map<std::string, uint64_t> copiedBefore;

        {
            std::unique_lock<std::mutex> lock (s->lock);

            // Bounded steps rather than one wait: cancel() is noticed promptly
            // even if its notify lands between a check and a wait.
            while (s->held.load (std::memory_order_acquire)
                   && ! s->cancelling.load (std::memory_order_acquire))
                s->changed.wait_for (lock, std::chrono::milliseconds (100));

            if (s->cancelling.load (std::memory_order_acquire)
                || (s->queued.empty() && s->scanRoot.empty()) || fs == nullptr)
            {
                s->running = false;
                s->changed.notify_all();
                return;
            }

            if (! s->scanRoot.empty())
            {
                scanRoot = s->scanRoot;
                s->scanRoot.clear();
            }
            else
            {
                key = s->queued.front();
                s->queued.pop_front();

                auto& record = s->records[key];
                record.phase = Phase::Copying;
                record.done = record.total = 0;
                record.againWhenDone = false;
                job = record.job;
                keepMarker = record.awaitingFollowUps;
                copiedBefore = record.copiedFiles;
                s->inFlight = key;
                s->lastProgress = Clock::now();
            }
        }

        if (! scanRoot.empty())
        {
            scan (*s, *fs, scanRoot);
            continue;
        }

        // Where the take is now, followed through a rename in Finder, and
        // said from the start: the status line and the saved-take card name
        // the folder as it is while the copy runs, not as it was at Stop.
        auto folder = job.localFolder;
        if (job.followedFolder != nullptr)
            if (const auto now = job.followedFolder->resolve(); ! now.empty())
                folder = now;

        // Not followed to anywhere else, and a later take now has the folder
        // it was started at: whatever is there is that take, never this one
        // to be copied under this one's name.
        bool folderTakenOver = false;

        {
            const std::lock_guard<std::mutex> lock (s->lock);
            s->records[key].folderNow = folder;

            const auto newest = s->newestAt.find (job.localFolder);
            folderTakenOver = folder == job.localFolder && newest != s->newestAt.end() && newest->second != key;
        }

        cardcopy::Control control;
        control.keepMarker = keepMarker;
        control.cancel = &s->cancelling;
        control.verified = &s->verified;
        control.copiedBefore = &copiedBefore;
        control.shouldYield = [&s] { return s->held.load (std::memory_order_acquire); };
        control.progress = [&s, &key] (uint64_t done, uint64_t total)
        {
            const std::lock_guard<std::mutex> lock (s->lock);
            auto& record = s->records[key];
            record.done = done;
            record.total = total;
            s->lastProgress = Clock::now();
        };

        cardcopy::Result result;

        if (folderTakenOver)
        {
            result.outcome = cardcopy::Outcome::ComputerCopyMissing;
            result.cardFolder = job.marker.cardFolder;
        }
        else
        {
            result = cardcopy::copyTake (*fs, folder, job.marker, control);
        }

        // A file on this computer that went missing mid-copy because the
        // folder was renamed in Finder: the folder is followed, and the copy
        // carries on from where it is now. What was checked on the card stays
        // checked, so nothing is written twice.
        std::string movedTo;
        if ((result.outcome == cardcopy::Outcome::ComputerCopyMissing
             || result.outcome == cardcopy::Outcome::ComputerReadFailed)
            && job.followedFolder != nullptr)
            if (const auto now = job.followedFolder->resolve(); ! now.empty() && now != folder)
                movedTo = now;

        const std::lock_guard<std::mutex> lock (s->lock);
        s->inFlight = 0;

        auto& record = s->records[key];

        if (! movedTo.empty() && record.movedFolderRetries < 3
            && ! s->cancelling.load (std::memory_order_acquire))
        {
            ++record.movedFolderRetries;
            record.folderNow = movedTo;
            if (! result.cardFolder.empty())
                record.job.marker.cardFolder = result.cardFolder;
            enqueueLocked (*s, key, true);
            continue;
        }

        record.movedFolderRetries = 0;

        // The folder it chose on the card, kept for the next attempt so a take
        // is never copied twice into "_2" and "_3".
        if (! result.cardFolder.empty())
            record.job.marker.cardFolder = result.cardFolder;

        if (result.outcome == cardcopy::Outcome::Yielded)
        {
            // A take started: back to the front, to finish after it stops.
            enqueueLocked (*s, key, true);
            continue;
        }

        if (result.outcome == cardcopy::Outcome::Cancelled)
        {
            // Quitting. The marker stays on this computer, so the next launch
            // picks it up again.
            record.phase = Phase::Waiting;
            continue;
        }

        const bool copied = result.outcome == cardcopy::Outcome::Copied;
        const bool repeat = sameAttempt (record.last, result);
        const bool quietResync = copied && record.resync && result.filesCopied == 0;

        // A take already copied whose folder on this computer has since been
        // deleted -- as the guide suggests once the card's copy is checked --
        // is still copied. Nothing to say about it.
        if (record.resync && result.outcome == cardcopy::Outcome::ComputerCopyMissing)
        {
            record.phase = Phase::Copied;
            record.resync = false;
            record.againWhenDone = false;
            continue;
        }

        record.phase = copied ? Phase::Copied : Phase::StillOnComputer;
        record.last = result;
        record.resync = false;
        if (copied)
            record.copiedFiles = result.filesOnCard;

        if (! quietResync)
            s->finished.push_back ({ job.localFolder, folder, result, repeat && ! copied });

        if (record.againWhenDone)
        {
            record.againWhenDone = false;
            record.resync = copied;
            enqueueLocked (*s, key);
        }
    }
}

} // namespace mma
