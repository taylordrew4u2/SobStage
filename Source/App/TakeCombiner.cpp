#include "TakeCombiner.h"
#include "../Core/FfmpegCommand.h"
#include "../Core/FfmpegLocator.h"
#include "../Platform/VolumeCapacity.h"
#include <chrono>

#if defined (MMA_NATIVE_MOVIE_COMBINER)
 #include "../Platform/MacMovieCombiner.h"
#endif

namespace mma {

TakeCombiner::TakeCombiner()
    : runState (std::make_shared<RunState>())
{
}

TakeCombiner::~TakeCombiner()
{
    cancel();
}

juce::String TakeCombiner::findFfmpeg()
{
    if (ffmpegOverride.isNotEmpty())
        return ffmpegOverride;

   #if defined (MMA_NATIVE_MOVIE_COMBINER)
    // The Mac does this itself. Asking people to install Homebrew and ffmpeg
    // before a show, for a feature the OS already has, was the one thing
    // standing between the switch and a file they could send.
    return kBuiltInCombiner;
   #endif

    if (resolvedFfmpeg.isNotEmpty())
        return resolvedFfmpeg;

    if (haveMissed && juce::Time::getMillisecondCounter() - lastMissMs < kFfmpegRetryMs)
        return {};

    return probeFfmpeg();
}

juce::String TakeCombiner::probeFfmpeg()
{
    if (ffmpegOverride.isNotEmpty())
        return ffmpegOverride;

   #if defined (MMA_NATIVE_MOVIE_COMBINER)
    return kBuiltInCombiner;
   #endif

    if (resolvedFfmpeg.isNotEmpty())
        return resolvedFfmpeg;

    const auto candidates = hasSearchPathsForTesting ? searchPathsForTesting
                                                     : ffmpegSearchPaths (thisHostPlatform());

    for (const auto& candidate : candidates)
    {
        const juce::String path (candidate);

        // A bare name is for PATH to answer, and the only honest way to ask is
        // to run it. Anything with a separator in it is a real location and can
        // be checked without spawning anything.
        if ((path.contains ("/") || path.contains ("\\")) && ! juce::File (path).existsAsFile())
            continue;

        // Run it, every candidate: existing is not the same as working. An
        // Intel ffmpeg migrated onto an Apple-silicon Mac without Rosetta
        // exists, was accepted, and failed every take -- while the faster
        // /opt/homebrew one installed later was never looked at. The output
        // is checked rather than the exit code, which a signal can read as 0;
        // -version prints a couple of KB, well inside the pipe.
        juce::ChildProcess probe;

        if (probe.start (juce::StringArray { path, "-version" }, juce::ChildProcess::wantStdOut))
        {
            if (! probe.waitForProcessToFinish (4000))
            {
                probe.kill();
                continue;
            }

            if (looksLikeFfmpegVersionOutput (probe.readAllProcessOutput().toStdString()))
            {
                resolvedFfmpeg = path;
                return resolvedFfmpeg;
            }
        }
    }

    haveMissed = true;
    lastMissMs = juce::Time::getMillisecondCounter();
    return {};
}

bool TakeCombiner::start (const juce::File& sessionFolder, const CombinedTakePlan& plan)
{
    if (! plan.hasWork())
        return false;

    // A take stopped while the previous one is still muxing waits its turn on
    // the worker already running, rather than being turned away: nothing else
    // would ever ask again, and its combined file would silently never appear.
    if (runState != nullptr)
    {
        const std::lock_guard<std::mutex> lock (runState->statusLock);

        if (runState->running.load (std::memory_order_acquire))
        {
            // Quitting: nothing more may start, queued or otherwise.
            if (runState->cancelling.load (std::memory_order_acquire))
                return false;

            runState->queued.push_back ({ sessionFolder, plan });
            runState->status.total += static_cast<int> (plan.jobs.size());
            return true;
        }
    }

    // A binary that failed the last combine is looked for again: a newer one
    // may have been installed at a better location since, and the broken one
    // must not be kept for the rest of the session.
    if (runState != nullptr)
    {
        const std::lock_guard<std::mutex> lock (runState->statusLock);
        if (runState->status.problem.isNotEmpty())
            resolvedFfmpeg.clear();
    }

    // Always a fresh look for a miss: this is the take the user was told
    // would combine once ffmpeg was installed.
    const auto ffmpeg = probeFfmpeg();
    auto next = std::make_shared<RunState>();

    // A take stopped and started again before this one has even begun: it
    // waits for that take too.
    next->held.store (heldForRecording, std::memory_order_release);
    next->availableBytes = availableBytesQuery != nullptr
        ? availableBytesQuery
        : [] (const std::string& path) { return availableBytesForRecording (path); };

    {
        const std::lock_guard<std::mutex> lock (next->statusLock);
        next->status.total = static_cast<int> (plan.jobs.size());
        next->status.run = ++lastRun;
    }

    if (ffmpeg.isEmpty())
    {
        // §10.6: name what happened and what to do about it. Nothing has been
        // lost -- the picture and the sound are both on disk, complete -- so
        // this says that too, or the sentence reads like a failed recording.
        const std::lock_guard<std::mutex> lock (next->statusLock);
        next->status.problem = "Couldn't find ffmpeg, so the combined video wasn't made. "
                               "Your picture and sound are both saved as separate files. "
                               "Install ffmpeg (on a Mac: brew install ffmpeg) and the next "
                               "take will combine them.";
        runState = std::move (next);
        return false;
    }

    next->running.store (true, std::memory_order_release);
    {
        const std::lock_guard<std::mutex> lock (next->statusLock);
        next->status.running = true;
    }
    runState = next;

    // A removable volume or ffmpeg itself may never answer. The worker owns
    // every path and all status it can touch, so quitting merely requests
    // cancellation and never joins it. A late worker cannot refer back to a
    // destroyed TakeCombiner or overwrite a later run's status.
    try
    {
        std::thread ([next, sessionFolder, plan, ffmpeg]
        {
            TakeCombiner::run (next, sessionFolder, plan, ffmpeg);
        }).detach();
    }
    catch (...)
    {
        next->running.store (false, std::memory_order_release);
        const std::lock_guard<std::mutex> lock (next->statusLock);
        next->status.running = false;
        next->status.problem = "Couldn't start the combined-video worker. Your separate picture "
                               "and sound files are still saved.";
        return false;
    }

    return true;
}

bool TakeCombiner::waitWhileHeld (RunState& state)
{
    std::unique_lock<std::mutex> lock (state.statusLock);

    // Bounded steps rather than one wait: cancel() only sets its flag, and
    // quitting must still be noticed promptly while a take holds the card.
    while (state.held.load (std::memory_order_acquire)
           && ! state.cancelling.load (std::memory_order_acquire))
        state.released.wait_for (lock, std::chrono::milliseconds (100));

    return ! state.cancelling.load (std::memory_order_acquire);
}

void TakeCombiner::combineTake (const std::shared_ptr<RunState>& state,
                                const juce::File& sessionFolder,
                                const CombinedTakePlan& plan,
                                const juce::String& ffmpeg,
                                int& failures,
                                juce::String& firstFailureDetail)
{
    for (const auto& job : plan.jobs)
    {
        auto ended = Attempt::Yielded;
        juce::String problem;

        // Round again for as long as takes keep starting mid-combine: the copy
        // a take interrupted was thrown away, and is made again from the start
        // once that take has stopped. Not counted against the run -- nothing
        // went wrong, the take simply came first.
        while (ended == Attempt::Yielded)
        {
            if (! waitWhileHeld (*state))
                return;

            problem.clear();
            ended = combineOnce (*state, sessionFolder, job, ffmpeg, problem);
        }

        if (ended == Attempt::Cancelled)
            return;

        if (ended == Attempt::Failed)
        {
            ++failures;

            if (firstFailureDetail.isEmpty())
                firstFailureDetail = problem;
        }

        const std::lock_guard<std::mutex> lock (state->statusLock);
        ++state->status.done;

        if (ended == Attempt::Written)
            state->status.written.add (sessionFolder.getChildFile (juce::String (job.outputFile)).getFileName());
    }
}

TakeCombiner::Attempt TakeCombiner::combineOnce (RunState& state,
                                                 const juce::File& sessionFolder,
                                                 const CombinedTakeJob& job,
                                                 const juce::String& ffmpeg,
                                                 juce::String& problem)
{
    const auto video = sessionFolder.getChildFile (juce::String (job.videoFile));
    const auto output = sessionFolder.getChildFile (juce::String (job.outputFile));

    // The mix as it is on the card: MIX.wav, and MIX_001.wav onwards if a
    // long take crossed the split limit. The plan names only the first;
    // how many followed is known only now the take is finished.
    std::vector<std::string> namesInFolder;
    for (const auto& file : sessionFolder.findChildFiles (juce::File::findFiles, false))
        namesInFolder.push_back (file.getFileName().toStdString());

    std::vector<std::string> audioPaths;
    uint64_t inputBytes = static_cast<uint64_t> (juce::jmax<juce::int64> (0, video.getSize()));

    for (const auto& part : splitPartsInOrder (job.audioFile, namesInFolder))
    {
        const auto file = sessionFolder.getChildFile (juce::String::fromUTF8 (part.c_str()));
        audioPaths.push_back (file.getFullPathName().toStdString());
        inputBytes += static_cast<uint64_t> (juce::jmax<juce::int64> (0, file.getSize()));
    }

    // Checked here rather than in the plan, because the plan is built from
    // what the take intended to write and this runs against what is
    // actually on the card -- which a pulled card or a full disk can make
    // two different things.
    if (! video.existsAsFile() || audioPaths.empty())
    {
        problem = juce::String (job.videoFile) + " or its sound wasn't on the card to combine.";
        return Attempt::Failed;
    }

    // The combined file is about the size of what it is made from. Started on
    // a card without that much room, the copy ran the card dry before failing.
    if (! hasRoomToCombine (state.availableBytes != nullptr
                                ? state.availableBytes (sessionFolder.getFullPathName().toStdString())
                                : std::nullopt,
                            inputBytes))
    {
        problem = "There wasn't room on the card for the combined copy of "
                + juce::String (job.videoFile) + ".";
        return Attempt::Failed;
    }

    // Asked while the copy runs: quitting, or a take starting on the card.
    bool yielded = false;
    const std::function<bool()> shouldStop = [&state, &yielded]
    {
        if (state.cancelling.load (std::memory_order_acquire))
            return true;

        if (state.held.load (std::memory_order_acquire))
            yielded = true;

        return yielded;
    };

   #if defined (MMA_NATIVE_MOVIE_COMBINER)
    if (ffmpeg == kBuiltInCombiner)
    {
        const auto why = combineMovieWithSound (video.getFullPathName().toStdString(),
                                                audioPaths,
                                                output.getFullPathName().toStdString(),
                                                job.audioLeadSeconds,
                                                shouldStop);

        if (state.cancelling.load (std::memory_order_acquire))
        {
            output.deleteFile();
            return Attempt::Cancelled;
        }

        if (why.empty())
            return Attempt::Written;

        output.deleteFile();

        if (yielded)
            return Attempt::Yielded;

        problem = juce::String::fromUTF8 (why.c_str());
        return Attempt::Failed;
    }
   #endif

    // More than one part is joined through a list file, kept in the temp
    // folder rather than the take's and removed whichever way this ends.
    // UTF-8 inside it, which is what ffmpeg reads it as on every platform.
    const auto concatList = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile ("sobstage-mix", ".ffconcat", false);

    struct RemoveList
    {
        const juce::File& file;
        ~RemoveList() { file.deleteFile(); }
    } removeList { concatList };

    if (audioPaths.size() > 1
        && ! concatList.replaceWithText (juce::String::fromUTF8 (buildFfmpegConcatList (audioPaths).c_str()),
                                         false, false, "\n"))
    {
        problem = "The list of the sound's parts couldn't be written.";
        return Attempt::Failed;
    }

    const auto args = buildFfmpegArguments (ffmpeg.toStdString(),
                                            video.getFullPathName().toStdString(),
                                            audioPaths,
                                            concatList.getFullPathName().toStdString(),
                                            output.getFullPathName().toStdString(),
                                            job.audioLeadSeconds,
                                            job.audioBitDepth);

    // Lowest disk priority on the Mac, so the next take recording to the
    // same card comes first. Only when the tool is where macOS keeps it.
    const auto platform = juce::File ("/usr/sbin/taskpolicy").existsAsFile()
                            ? thisHostPlatform() : HostPlatform::Linux;

    juce::StringArray argv;
    for (const auto& arg : withLowDiskPriority (args, platform))
        argv.add (juce::String (arg));

    juce::ChildProcess process;

    // No captured output pipe: POSIX stdio reads can block forever while
    // a wedged ffmpeg still owns the pipe. With its streams discarded we
    // can poll the child in bounded steps and honour shutdown promptly.
    if (! process.start (argv, 0))
    {
        output.deleteFile();
        problem = "ffmpeg wouldn't start.";
        return Attempt::Failed;
    }

    bool finished = false;

    while (! shouldStop())
    {
        if (process.waitForProcessToFinish (100))
        {
            finished = true;
            break;
        }
    }

    // A file left behind by a run that was stopped or failed halfway is worse
    // than no file at all: it plays, briefly, and looks like the take.
    if (! finished || state.cancelling.load (std::memory_order_acquire))
    {
        if (! finished)
        {
            if (process.isRunning())
                (void) process.kill();

            (void) process.waitForProcessToFinish (1000);
        }

        output.deleteFile();
        return state.cancelling.load (std::memory_order_acquire) ? Attempt::Cancelled
                                                                 : Attempt::Yielded;
    }

    // waitForProcessToFinish's successful terminal poll is also the
    // POSIX reap. Calling isRunning() after it can ask waitpid about an
    // already-reaped child and make JUCE replace the real status.
    if (process.getExitCode() != 0)
    {
        output.deleteFile();
        problem = "ffmpeg stopped before it made a complete file.";
        return Attempt::Failed;
    }

    return Attempt::Written;
}

void TakeCombiner::run (std::shared_ptr<RunState> state,
                        juce::File sessionFolder,
                        CombinedTakePlan plan,
                        juce::String ffmpeg)
{
    int failures = 0;

    // Kept from the first failure only. Two cameras failing for the same reason
    // say it once; two failing for different reasons are still one sentence,
    // and the first is the one that stopped the run being clean.
    juce::String firstFailureDetail;

    for (;;)
    {
        combineTake (state, sessionFolder, plan, ffmpeg, failures, firstFailureDetail);

        // The next take stopped while this one was muxing, or the end of the
        // run. Decided under the lock start() queues under, and the run is
        // marked finished before that lock is let go, so a take queued at this
        // instant is either picked up here or finds the run over and starts
        // its own. Never neither.
        const std::lock_guard<std::mutex> lock (state->statusLock);

        if (! state->queued.empty() && ! state->cancelling.load (std::memory_order_acquire))
        {
            sessionFolder = state->queued.front().sessionFolder;
            plan = std::move (state->queued.front().plan);
            state->queued.pop_front();
            continue;
        }

        state->queued.clear();
        state->status.running = false;

        if (failures > 0)
        {
            state->status.problem = juce::String (failures)
                                  + (failures == 1 ? " camera couldn't be combined with the sound. "
                                                   : " cameras couldn't be combined with the sound. ")
                                  + "The separate picture and sound files are all still there.";

            if (firstFailureDetail.isNotEmpty())
                state->status.problem += " (" + firstFailureDetail + ")";
        }

        state->running.store (false, std::memory_order_release);
        return;
    }
}

void TakeCombiner::cancel() noexcept
{
    if (runState != nullptr)
    {
        runState->cancelling.store (true, std::memory_order_release);
        runState->released.notify_all();
    }
}

void TakeCombiner::setHeldForRecording (bool held)
{
    heldForRecording = held;

    if (runState == nullptr || runState->held.load (std::memory_order_acquire) == held)
        return;

    {
        // Under the lock the worker waits under, so a release is never
        // missed between its check and its wait.
        const std::lock_guard<std::mutex> lock (runState->statusLock);
        runState->held.store (held, std::memory_order_release);
    }

    runState->released.notify_all();
}

bool TakeCombiner::isRunning() const
{
    return runState != nullptr
        && runState->running.load (std::memory_order_acquire);
}

TakeCombiner::Status TakeCombiner::getStatus() const
{
    const auto state = runState;

    if (state == nullptr)
        return {};

    const std::lock_guard<std::mutex> lock (state->statusLock);
    return state->status;
}

} // namespace mma
