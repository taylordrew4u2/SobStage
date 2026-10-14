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

void CardCopier::expect (const std::string& localFolder)
{
    const std::lock_guard<std::mutex> lock (state->lock);

    if (! localFolder.empty())
        state->expected.insert (localFolder);
}

bool CardCopier::start (Job job)
{
    bool needed = false;

    {
        const std::lock_guard<std::mutex> lock (state->lock);

        if (state->cancelling.load (std::memory_order_acquire))
            return false;

        state->expected.erase (job.localFolder);

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

bool CardCopier::forReservedCardLocked (const State& s, const Record& record)
{
    if (s.reservedRoot.empty())
        return false;

    return record.job.marker.destinationRoot == s.reservedRoot
        || (! s.reservedIdentity.empty() && record.job.marker.cardIdentity == s.reservedIdentity);
}

void CardCopier::setCardReserved (const std::string& destinationRoot, const std::string& cardIdentity)
{
    {
        const std::lock_guard<std::mutex> lock (state->lock);

        if (state->reservedRoot == destinationRoot
            && state->reservedIdentity == (destinationRoot.empty() ? std::string() : cardIdentity))
            return;

        state->reservedRoot = destinationRoot;
        state->reservedIdentity = destinationRoot.empty() ? std::string() : cardIdentity;
    }

    state->changed.notify_all();
}

bool CardCopier::hasCopyToCardUnderWayOrDue (const std::string& destinationRoot,
                                             const std::string& cardIdentity) const
{
    const std::lock_guard<std::mutex> lock (state->lock);

    const auto forThatCard = [&] (TakeId id)
    {
        const auto found = state->records.find (id);
        if (found == state->records.end())
            return false;

        const auto& marker = found->second.job.marker;
        return marker.destinationRoot == destinationRoot
            || (! cardIdentity.empty() && marker.cardIdentity == cardIdentity);
    };

    // One in flight writes to the card until it reaches its next chunk, held
    // or reserved; one queued starts any moment unless something holds it.
    if (state->inFlight != 0 && forThatCard (state->inFlight))
        return true;

    if (state->held.load (std::memory_order_acquire)
        || (state->reservedRoot == destinationRoot && ! destinationRoot.empty()))
        return false;

    return std::any_of (state->queued.begin(), state->queued.end(), forThatCard);
}

bool CardCopier::isWorkLocked (const Record& record)
{
    // A retry of a card that was out, full or locked is not work a quit cuts
    // short: the marker keeps it for the next launch either way, and asking
    // would hold every quit while the card stays out. One queued because its
    // card was seen back in is: the user put it in for this.
    return ! record.last.has_value() || ! cardcopy::isFailure (record.last->outcome) || record.countsAsWork;
}

bool CardCopier::hasWorkToFinish() const
{
    const std::lock_guard<std::mutex> lock (state->lock);

    if (state->inFlight != 0 || ! state->scanRoot.empty())
        return true;

    for (const auto id : state->queued)
    {
        const auto found = state->records.find (id);

        if (found == state->records.end() || isWorkLocked (found->second))
            return true;
    }

    return false;
}

bool CardCopier::isAddingFilesOnlyLocked (const Record& record)
{
    return record.resync
        || (record.last.has_value() && cardcopy::isFailure (record.last->outcome) && record.last->addedFilesOnly);
}

std::pair<int, int> CardCopier::countWorkLocked() const
{
    int whole = 0, addingFiles = 0;

    const auto count = [&] (const Record* record)
    {
        if (record != nullptr && isAddingFilesOnlyLocked (*record))
            ++addingFiles;
        else
            ++whole;
    };

    if (state->inFlight != 0)
    {
        const auto found = state->records.find (state->inFlight);
        count (found != state->records.end() ? &found->second : nullptr);
    }

    for (const auto id : state->queued)
    {
        if (id == state->inFlight)
            continue;

        const auto found = state->records.find (id);

        if (found == state->records.end())
            count (nullptr);
        else if (isWorkLocked (found->second))
            count (&found->second);
    }

    return { whole, addingFiles };
}

int CardCopier::takesToFinish() const
{
    const std::lock_guard<std::mutex> lock (state->lock);
    return countWorkLocked().first;
}

int CardCopier::takesAddingFilesToFinish() const
{
    const std::lock_guard<std::mutex> lock (state->lock);
    return countWorkLocked().second;
}

int CardCopier::takesNotOnCard() const
{
    const std::lock_guard<std::mutex> lock (state->lock);
    int takes = 0;

    for (const auto& [id, record] : state->records)
    {
        if (id == state->inFlight || ! record.last.has_value() || ! cardcopy::isFailure (record.last->outcome)
            || record.last->outcome == cardcopy::Outcome::ComputerCopyMissing
            || record.last->outcome == cardcopy::Outcome::FileTooBig || record.last->addedFilesOnly)
            continue;

        if (record.phase == Phase::StillOnComputer
            || (record.phase == Phase::Waiting && ! isWorkLocked (record)))
            ++takes;
    }

    return takes;
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

int CardCopier::retryWithCardIn (const std::string& destinationRoot, const std::string& identitySeen,
                                 bool onlyWaitingForCard)
{
    int queuedNow = 0;
    bool needed = false;

    {
        const std::lock_guard<std::mutex> lock (state->lock);

        if (state->cancelling.load (std::memory_order_acquire) || destinationRoot.empty())
            return 0;

        for (auto& [id, record] : state->records)
        {
            if (record.job.marker.destinationRoot != destinationRoot || ! record.last.has_value()
                || ! cardcopy::isFailure (record.last->outcome)
                || record.last->outcome == cardcopy::Outcome::ComputerCopyMissing)
                continue;

            const auto outcome = record.last->outcome;
            const bool waitingForCard = outcome == cardcopy::Outcome::CardMissing
                                     || outcome == cardcopy::Outcome::DifferentCard;

            if (onlyWaitingForCard && ! waitingForCard)
                continue;

            // Another card than its own, still: nothing would go.
            if (outcome == cardcopy::Outcome::DifferentCard && ! identitySeen.empty()
                && ! record.job.marker.cardIdentity.empty() && identitySeen != record.job.marker.cardIdentity)
                continue;

            if (record.phase == Phase::StillOnComputer)
                needed = enqueueLocked (*state, id) || needed;
            else if (record.phase != Phase::Waiting)
                continue;

            record.countsAsWork = true;
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
        auto* found = newestLocked (*state, localFolder, &id);
        if (found == nullptr || found->phase == Phase::Waiting
            || found->phase == Phase::Copying || found->job.marker.destinationRoot.empty())
            return false;

        found->anyCard = true;
        needed = enqueueLocked (*state, id);
    }

    startWorkerIfNeeded (needed);
    return true;
}

namespace {

bool waitingForAnotherCard (const cardcopy::Result& last)
{
    return last.outcome == cardcopy::Outcome::DifferentCard;
}

} // namespace

int CardCopier::takesForAnotherCard (const std::string& destinationRoot, const std::string& identitySeen) const
{
    const std::lock_guard<std::mutex> lock (state->lock);
    int takes = 0;

    for (const auto& [id, record] : state->records)
        if (id != state->inFlight && record.job.marker.destinationRoot == destinationRoot
            && (record.phase == Phase::StillOnComputer || record.phase == Phase::Waiting)
            && record.last.has_value() && waitingForAnotherCard (*record.last) && ! record.anyCard
            && (identitySeen.empty() || record.job.marker.cardIdentity != identitySeen))
            ++takes;

    return takes;
}

int CardCopier::copyToCardInNow (const std::string& destinationRoot)
{
    int queuedNow = 0;
    bool needed = false;

    {
        const std::lock_guard<std::mutex> lock (state->lock);

        if (state->cancelling.load (std::memory_order_acquire) || destinationRoot.empty())
            return 0;

        for (auto& [id, record] : state->records)
        {
            if (id == state->inFlight || record.job.marker.destinationRoot != destinationRoot
                || ! record.last.has_value() || ! waitingForAnotherCard (*record.last))
                continue;

            if (record.phase == Phase::StillOnComputer)
                needed = enqueueLocked (*state, id) || needed;
            else if (record.phase != Phase::Waiting)
                continue;

            // Queued already -- by a timed retry -- it takes the card in when
            // it runs. Asked for, a quit waits for it.
            record.anyCard = true;
            record.countsAsWork = true;
            ++queuedNow;
        }
    }

    startWorkerIfNeeded (needed);
    return queuedNow;
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
        const auto where = record.folderNow.empty() ? record.job.localFolder : record.folderNow;

        if (id == state->inFlight)
        {
            s.copyingFolder = where;
            s.done = record.done;
            s.total = record.total;
            s.foldersStillToCopy.push_back (where);
        }
        else if (record.phase == Phase::Waiting)
        {
            ++s.waiting;
            s.foldersStillToCopy.push_back (where);
        }
        else if (record.phase == Phase::StillOnComputer && record.last.has_value()
                 && record.last->outcome != cardcopy::Outcome::ComputerCopyMissing)
        {
            s.foldersStillToCopy.push_back (where);

            // Everything else checked on the card: the saved-take card says
            // "The rest of the take was", and this agrees.
            if (record.last->outcome == cardcopy::Outcome::FileTooBig)
            {
                ++s.partlyOnCard;
                continue;
            }

            if (record.last->addedFilesOnly)
            {
                ++s.addedFilesWaiting;
                continue;
            }

            ++s.onlyOnComputer;

            // Both wait for the take's own card to be put in, but "when it is
            // back" of another card in its place would read as that one.
            if (record.last->outcome == cardcopy::Outcome::CardMissing)
                ++s.cardMissing;
            else if (record.last->outcome == cardcopy::Outcome::DifferentCard)
                ++s.differentCard;
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

    // Each folder held, as a take stopped in this run's is, so a rename in
    // Finder while it copies is followed rather than called a folder gone.
    // Here, on the worker: the folders are on this computer, but still disk
    // calls. A platform that cannot follow one holds nothing (null).
    std::vector<std::shared_ptr<TakeFolderTracker>> followed;
    std::vector<std::string> foundIdentity;

    for (const auto& [folder, marker] : found)
    {
        auto tracker = std::make_shared<TakeFolderTracker>();
        followed.push_back (tracker->open (folder) ? std::move (tracker) : nullptr);
        foundIdentity.push_back (followed.back() != nullptr ? followed.back()->fileIdentity() : std::string());
    }

    // Where every take this run already knows is now, and which folder on
    // the disk it is: one stopped while the copies were held -- behind the
    // card's check for interrupted takes -- and renamed in Finder since is
    // found here under its new name, and was copied a second time, into
    // "<name>_2", as "an earlier take". Asked of the folders themselves,
    // outside the lock, and asked again if a take was started meanwhile.
    std::set<std::string> knownPaths, knownFolders;
    std::unique_lock<std::mutex> lock (s.lock, std::defer_lock);

    for (int round = 0;; ++round)
    {
        std::vector<std::shared_ptr<TakeFolderTracker>> trackers;
        TakeId knownUpTo = 0;
        {
            const std::lock_guard<std::mutex> look (s.lock);
            knownUpTo = s.nextId;
            for (const auto& [id, record] : s.records)
                if (record.job.followedFolder != nullptr)
                    trackers.push_back (record.job.followedFolder);
        }

        for (const auto& t : trackers)
        {
            if (const auto now = t->resolve(); ! now.empty())
                knownPaths.insert (now);
            if (const auto identity = t->fileIdentity(); ! identity.empty())
                knownFolders.insert (identity);
        }

        lock.lock();
        if (s.nextId == knownUpTo || round >= 3)
            break;
        lock.unlock();
    }

    int added = 0;

    for (size_t i = 0; i < found.size(); ++i)
    {
        auto& [folder, marker] = found[i];

        // This run already knows it: started at a Stop, found before, or a
        // take of this run still to be started at its Stop.
        if (s.newestAt.count (folder) > 0 || s.expected.count (folder) > 0)
            continue;

        // Or it is one of those, under the name it has now.
        if (knownPaths.count (folder) > 0 || (! foundIdentity[i].empty() && knownFolders.count (foundIdentity[i]) > 0))
            continue;

        Job job;
        job.localFolder = folder;
        job.marker = std::move (marker);
        job.followedFolder = std::move (followed[i]);
        enqueueLocked (s, addTakeLocked (s, std::move (job)));
        ++added;
    }

    s.resumedCount = added;
}

void CardCopier::recopyForCardTakenLocked (State& s, TakeId taker, const std::string& cardBefore)
{
    const auto& root = s.records[taker].job.marker.destinationRoot;
    const auto cardNow = s.records[taker].job.marker.cardIdentity;
    int queuedNow = 0;

    for (auto& [id, record] : s.records)
    {
        if (id == taker || record.job.marker.destinationRoot != root
            || record.job.marker.cardIdentity != cardBefore
            || (record.last.has_value() && record.last->outcome == cardcopy::Outcome::ComputerCopyMissing))
            continue;

        // Checked on the card that went: nothing of it is trusted on this one.
        if (! record.job.marker.cardFolder.empty())
        {
            const auto prefix = cardcopy::joinPath (record.job.marker.cardFolder, {});
            for (auto it = s.verified.begin(); it != s.verified.end();)
                it = it->compare (0, prefix.size(), prefix) == 0 ? s.verified.erase (it) : std::next (it);
        }

        // For this card now, in a folder it chooses on it.
        record.job.marker.cardIdentity = cardNow;
        record.job.marker.cardFolder.clear();
        record.job.marker.takeNextCard = false;
        record.copiedFiles.clear();
        record.resync = false;

        const auto folder = record.folderNow.empty() ? record.job.localFolder : record.folderNow;

        if (record.phase == Phase::Copied || record.phase == Phase::StillOnComputer)
        {
            // Not on this card: a copy a quit would cut off, said as one.
            record.countsAsWork = true;
            s.notesToWrite.emplace_back (folder, record.job.marker);
            enqueueLocked (s, id);
            ++queuedNow;
        }
        else if (record.phase == Phase::Waiting)
        {
            s.notesToWrite.emplace_back (folder, record.job.marker);
        }
    }

    if (queuedNow > 0)
        s.recopiedCount = s.recopiedCount.value_or (0) + queuedNow;
}

std::optional<int> CardCopier::takeRecopiedCount()
{
    const std::lock_guard<std::mutex> lock (state->lock);
    auto count = state->recopiedCount;
    state->recopiedCount.reset();
    return count;
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
        // The notes on this computer for takes queued again for another card
        // (recopyForCardTakenLocked): a quit before their copy then still has
        // them copied at the next launch. Disk calls, so outside the lock.
        {
            std::vector<std::pair<std::string, cardcopy::Marker>> notes;
            {
                const std::lock_guard<std::mutex> lock (s->lock);
                notes.swap (s->notesToWrite);
            }

            for (const auto& [folder, marker] : notes)
                if (fs != nullptr && fs->exists (folder))
                    (void) fs->replaceText (cardcopy::joinPath (folder, cardcopy::kMarkerFileName),
                                            cardcopy::markerToJson (marker));
        }

        TakeId key = 0;
        Job job;
        std::string scanRoot;
        bool keepMarker = false;
        bool anyCard = false;
        std::map<std::string, uint64_t> copiedBefore;

        {
            std::unique_lock<std::mutex> lock (s->lock);

            // The first queued take not for a card reserved for its speed
            // test (setCardReserved): copies to other cards go on meanwhile.
            const auto nextFree = [&s]
            {
                return std::find_if (s->queued.begin(), s->queued.end(), [&s] (TakeId id)
                                     { return ! forReservedCardLocked (*s, s->records[id]); });
            };

            // Bounded steps rather than one wait: cancel() is noticed promptly
            // even if its notify lands between a check and a wait.
            while (! s->cancelling.load (std::memory_order_acquire)
                   && (s->held.load (std::memory_order_acquire)
                       || (s->scanRoot.empty() && ! s->queued.empty() && nextFree() == s->queued.end())))
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
                const auto next = nextFree();
                key = *next;
                s->queued.erase (next);

                auto& record = s->records[key];
                record.phase = Phase::Copying;
                record.done = record.total = 0;
                record.againWhenDone = false;
                job = record.job;
                keepMarker = record.awaitingFollowUps;
                copiedBefore = record.copiedFiles;
                // A file too big for its card takes the next card itself
                // (Marker::takeNextCard), in the note on this computer too.
                anyCard = record.anyCard;
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
        control.takeIsFinished = job.stoppedHere;
        control.cancel = &s->cancelling;
        control.verified = &s->verified;
        control.copiedBefore = &copiedBefore;
        control.anyCard = anyCard;
        // Held for a take, or this card reserved for its speed test.
        control.shouldYield = [&s, &key]
        {
            if (s->held.load (std::memory_order_acquire))
                return true;

            const std::lock_guard<std::mutex> lock (s->lock);
            return forReservedCardLocked (*s, s->records[key]);
        };
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
            if (! result.cardIdentity.empty() && result.cardIdentity != record.job.marker.cardIdentity)
                record.job.marker.takeNextCard = false;
            if (! result.cardIdentity.empty())
                record.job.marker.cardIdentity = result.cardIdentity;
            enqueueLocked (*s, key, true);
            continue;
        }

        record.movedFolderRetries = 0;

        // The folder it chose on the card, kept for the next attempt so a take
        // is never copied twice into "_2" and "_3"; and the card it is for.
        const bool cardTaken = ! result.cardIdentity.empty()
                            && result.cardIdentity != record.job.marker.cardIdentity;
        const bool tookTheNextCard = cardTaken && record.job.marker.takeNextCard;
        const auto cardBefore = record.job.marker.cardIdentity;

        if (! result.cardFolder.empty())
            record.job.marker.cardFolder = result.cardFolder;

        if (! result.cardIdentity.empty())
        {
            if (cardTaken)
                record.job.marker.cardFolder = result.cardFolder;

            record.job.marker.cardIdentity = result.cardIdentity;
        }

        // Taken or not, "Copy again" was for this attempt only, and so was
        // the card seen back in.
        if (result.outcome != cardcopy::Outcome::Yielded)
        {
            record.anyCard = false;
            record.countsAsWork = false;
        }

        // Told to use another card or reformat this one: the next card
        // reached is the take's, as the note on this computer says too
        // (copyTake keeps the two alike). Kept through every attempt until a
        // card is taken or the take is copied -- the same card put back full
        // or locked is not the card the user was sent to find.
        if (result.outcome == cardcopy::Outcome::FileTooBig)
            record.job.marker.takeNextCard = true;
        else if (cardTaken || result.outcome == cardcopy::Outcome::Copied)
            record.job.marker.takeNextCard = false;

        // The card that file was too big for has been replaced -- by another
        // card, or by itself reformatted as the user was told, which erased
        // every take on it. Which of the two cannot be told, so every other
        // take of this run that went to it, or was waiting for it, at this
        // save location goes to this card too: copied and checked again, and
        // counted as waiting until it is. A take that stayed "Copied to the
        // card" read as safe on a card that had been erased.
        if (tookTheNextCard && ! cardBefore.empty())
            recopyForCardTakenLocked (*s, key, cardBefore);

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

        // A card first seen by an attempt that then failed is still the one
        // the take went to, when a later attempt copies it.
        record.cardFirstSeen = record.cardFirstSeen || result.cardFirstSeen;
        if (copied)
        {
            result.cardFirstSeen = record.cardFirstSeen;
            record.cardFirstSeen = false;
        }

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
