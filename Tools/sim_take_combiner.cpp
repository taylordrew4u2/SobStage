#include "App/TakeCombiner.h"
#include "Core/SessionWriter.h"
#include "Core/TakeCopy.h"
#include <chrono>
#include <cstring>
#include <cstdio>
#include <memory>
#include <thread>

namespace {

int fail (const char* message)
{
    std::fprintf (stderr, "FAIL  %s\n", message);
    return 1;
}

/// Runs a real ffmpeg to completion, output discarded. False on any failure.
bool runToCompletion (const juce::StringArray& argv)
{
    juce::ChildProcess process;
    return process.start (argv, 0)
        && process.waitForProcessToFinish (8000)
        && process.getExitCode() == 0;
}

} // namespace

namespace mma {

struct SessionWriterTestAccess
{
    static void useSplitSize (SessionWriter& writer, uint64_t bytes)
    {
        writer.setAutoSplitBytesForTesting (bytes);
    }
};

} // namespace mma

namespace {

/// A mix that rolled over into MIX_001.wav, MIX_002.wav... must reach the
/// combined file whole. The plan named MIX.wav alone and ffmpeg was told to
/// stop at the shorter stream, so a take long enough to split (3.9 GB, a few
/// hours) came back as a video cut off where the first part ended.
///
/// Uses the real ffmpeg, because what is being checked is what ffmpeg does
/// with the arguments -- not what they look like. Skipped, loudly, on a
/// machine without one.
int checkSplitMixReachesTheCombinedFile (const juce::File& root)
{
    mma::TakeCombiner locator;
    const auto ffmpeg = locator.findFfmpeg();

    if (ffmpeg.isEmpty())
    {
        std::printf ("SKIP  no ffmpeg on this machine; the split-mix combine was not run\n");
        return 0;
    }

    const auto take = root.getChildFile ("split-take");
    if (! take.createDirectory().wasOk())
        return fail ("could not make the split take's folder");

    constexpr double kRate = 48000.0;
    constexpr double kSeconds = 6.0;
    constexpr double kLead = 0.5;

    // Six seconds of picture, as a camera would have written it.
    if (! runToCompletion ({ ffmpeg, "-nostdin", "-loglevel", "error", "-y",
                             "-f", "lavfi", "-i", "testsrc=size=160x120:rate=25",
                             "-t", "6", "-c:v", "mpeg4",
                             take.getChildFile ("V01_Camera.mov").getFullPathName() }))
        return fail ("could not make the split take's fake video");

    int parts = 0;

    // Six seconds of mix, split after about two: mono 24-bit is 144 kB a
    // second, so this is MIX.wav plus MIX_001.wav and MIX_002.wav.
    {
        mma::SessionWriter writer;
        mma::SessionWriterTestAccess::useSplitSize (writer, 300000);

        if (! writer.open (take.getChildFile ("MIX").getFullPathName().toStdString(),
                           kRate, 1, 24, "2026-09-29T12:00:00Z"))
            return fail ("could not open the split take's mix");

        std::vector<float> block (4800, 0.25f);
        for (int i = 0; i < static_cast<int> (kSeconds * kRate) / 4800; ++i)
            if (! writer.writeInterleaved (block.data(), block.size()))
                return fail ("could not write the split take's mix");

        parts = writer.getSplitFileCount() + 1;

        if (! writer.close() || parts < 3)
            return fail ("the mix did not split the way the check needs it to");
    }

    mma::CombinedTakePlan plan;
    plan.jobs.push_back ({ "V01_Camera.mov", "MIX.wav",
                           "V01_Camera_with-sound.mov", 24, kLead });

    mma::TakeCombiner combiner;
    if (! combiner.start (take, plan))
        return fail ("the split take's real combine did not start");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (6);
    while (combiner.isRunning() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    if (combiner.isRunning())
        return fail ("the split take's real combine did not finish");

    const auto status = combiner.getStatus();
    if (status.problem.isNotEmpty() || ! status.written.contains ("V01_Camera_with-sound.mov"))
        return fail ("the split take's real combine reported a failure");

    // Decoded back to bare 16-bit mono: the byte count is the duration.
    const auto decoded = take.getChildFile ("decoded.raw");
    if (! runToCompletion ({ ffmpeg, "-nostdin", "-loglevel", "error", "-y",
                             "-i", take.getChildFile ("V01_Camera_with-sound.mov").getFullPathName(),
                             "-map", "0:a:0", "-f", "s16le", "-ac", "1", "-ar", "48000",
                             decoded.getFullPathName() }))
        return fail ("could not read the sound back out of the combined file");

    const double audioSeconds = static_cast<double> (decoded.getSize()) / 2.0 / kRate;
    std::printf ("  split mix: %d parts, combined sound %.2f s of %.2f expected\n",
                 parts, audioSeconds, kSeconds - kLead);

    if (audioSeconds < kSeconds - kLead - 0.25)
        return fail ("the combined file's sound stopped where the mix's first part ended");

    // Nothing of the combine's own left in the take folder.
    for (const auto& entry : take.findChildFiles (juce::File::findFiles, false))
        if (entry.getFileExtension() != ".wav" && entry.getFileExtension() != ".mov"
            && entry.getFileExtension() != ".raw")
            return fail ("the combine left a working file in the take folder");

    return 0;
}

} // namespace

int main (int argc, char** argv)
{
    // TakeCombiner invokes this executable as its stand-in for ffmpeg. A real
    // wedged muxer never exits; the parent must still be able to destroy the
    // combiner immediately and its detached worker must kill this child.
    if (argc > 1)
    {
        // Answers the lookup's "does it really run" check the way ffmpeg does.
        if (argc == 2 && std::strcmp (argv[1], "-version") == 0)
        {
            std::puts ("ffmpeg version sim-stand-in");
            return 0;
        }

        for (int index = 1; index < argc; ++index)
        {
            if (std::strstr (argv[index], "success-input") != nullptr)
            {
                if (auto* complete = std::fopen (argv[argc - 1], "wb"))
                {
                    std::fputs ("complete", complete);
                    std::fclose (complete);
                    return 0;
                }

                return 8;
            }

            // A combine that takes a while and then succeeds: long enough that
            // the next take can be stopped while this one is still running.
            if (std::strstr (argv[index], "slow-input") != nullptr)
            {
                std::this_thread::sleep_for (std::chrono::milliseconds (400));

                if (auto* complete = std::fopen (argv[argc - 1], "wb"))
                {
                    std::fputs ("complete", complete);
                    std::fclose (complete);
                    return 0;
                }

                return 8;
            }

            // A combine that is seen half-written: the file ffmpeg was told
            // to write has something in it well before it is finished.
            if (std::strstr (argv[index], "watched-input") != nullptr)
            {
                if (auto* growing = std::fopen (argv[argc - 1], "wb"))
                {
                    std::fputs ("half", growing);
                    std::fflush (growing);
                    std::this_thread::sleep_for (std::chrono::milliseconds (600));
                    std::fputs (" and the rest", growing);
                    std::fclose (growing);
                    return 0;
                }

                return 8;
            }

            if (std::strstr (argv[index], "fail-input") != nullptr)
            {
                if (auto* partial = std::fopen (argv[argc - 1], "wb"))
                {
                    std::fputs ("partial", partial);
                    std::fclose (partial);
                }

                return 7;
            }
        }

        for (;;)
            std::this_thread::sleep_for (std::chrono::seconds (1));
    }

    // argv[0] may be relative when this probe is launched directly. Resolve it
    // against the current directory instead of tripping JUCE's absolute-path
    // assertion in a successful test run.
    const auto executable = juce::File::getCurrentWorkingDirectory()
                                .getChildFile (juce::String::fromUTF8 (argv[0]))
                                .getFullPathName();
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getNonexistentChildFile ("sobstage-combiner-probe", {}, false);

    if (! root.createDirectory().wasOk())
        return fail ("could not create the temporary take folder");

    struct Cleanup
    {
        juce::File root;
        ~Cleanup() { root.deleteRecursively(); }
    } cleanup { root };

    const auto video = root.getChildFile ("V01_Camera.mov");
    const auto audio = root.getChildFile ("MIX.wav");

    if (! video.replaceWithText ("video") || ! audio.replaceWithText ("audio"))
        return fail ("could not make the fake take inputs");

    mma::CombinedTakePlan plan;
    plan.jobs.push_back ({ "V01_Camera.mov", "MIX.wav",
                           "V01_Camera_with-sound.mov", 24, 0.0 });

    auto combiner = std::make_unique<mma::TakeCombiner>();
    combiner->setFfmpegOverride (executable);

    if (! combiner->start (root, plan) || ! combiner->isRunning())
        return fail ("the simulated combine did not start");

    // Give the worker enough time to enter its infinite child. This is not a
    // timing assertion; only destruction below is timed.
    std::this_thread::sleep_for (std::chrono::milliseconds (250));

    const auto before = std::chrono::steady_clock::now();
    combiner.reset();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - before);

    if (elapsed > std::chrono::milliseconds (100))
        return fail ("destroying the combiner waited for the wedged child");

    // The detached owner gets a bounded poll in which to observe cancellation,
    // kill the child and discard any incomplete output.
    std::this_thread::sleep_for (std::chrono::milliseconds (350));

    if (root.getChildFile ("V01_Camera_with-sound.mov").exists()
        || root.getChildFile (".V01_Camera_with-sound.mov").exists())
        return fail ("a cancelled combine left an output file behind");

    const auto failingVideo = root.getChildFile ("fail-input.mov");
    if (! failingVideo.replaceWithText ("video"))
        return fail ("could not make the failing fake input");

    mma::CombinedTakePlan failingPlan;
    failingPlan.jobs.push_back ({ "fail-input.mov", "MIX.wav",
                                  "failed-output.mov", 24, 0.0 });

    mma::TakeCombiner failingCombiner;
    failingCombiner.setFfmpegOverride (executable);

    if (! failingCombiner.start (root, failingPlan))
        return fail ("the failing simulated combine did not start");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
    while (failingCombiner.isRunning() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    if (failingCombiner.isRunning())
        return fail ("the failing simulated combine did not finish");

    const auto failedStatus = failingCombiner.getStatus();
    if (failedStatus.problem.isEmpty())
        return fail ("a non-zero ffmpeg exit was reported as success");

    if (! failedStatus.written.isEmpty())
        return fail ("a non-zero ffmpeg exit was added to written outputs");

    if (root.getChildFile ("failed-output.mov").exists() || root.getChildFile (".failed-output.mov").exists())
        return fail ("a non-zero ffmpeg exit left its partial output behind");

    // A copy to the card lists the take's folder while the combine runs. The
    // half-made movie must not be among what it would carry: Windows refuses
    // to open a file ffmpeg is still writing, and the copy called the whole
    // take's folder gone. It appears under its real name only when finished.
    {
        const auto watched = root.getChildFile ("watched-input.mov");
        if (! watched.replaceWithText ("video"))
            return fail ("could not make the watched fake input");

        mma::CombinedTakePlan watchedPlan;
        watchedPlan.jobs.push_back ({ "watched-input.mov", "MIX.wav",
                                      "watched_with-sound.mov", 24, 0.0 });

        mma::TakeCombiner watchedCombiner;
        watchedCombiner.setFfmpegOverride (executable);

        if (! watchedCombiner.start (root, watchedPlan))
            return fail ("the watched simulated combine did not start");

        std::this_thread::sleep_for (std::chrono::milliseconds (300));

        bool sawHalfMadeMovie = false;
        for (const auto& file : root.findChildFiles (juce::File::findFiles, false))
            if (file.getFileName().contains ("watched_with-sound")
                && mma::cardcopy::shouldCopy (file.getFileName().toStdString()))
                sawHalfMadeMovie = true;

        if (! watchedCombiner.isRunning())
            return fail ("the watched combine finished before it could be looked at");

        if (sawHalfMadeMovie)
            return fail ("a combined movie still being made was in the take's folder under a name the card copy carries");

        const auto watchDeadline = std::chrono::steady_clock::now() + std::chrono::seconds (3);
        while (watchedCombiner.isRunning() && std::chrono::steady_clock::now() < watchDeadline)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));

        if (root.getChildFile ("watched_with-sound.mov").loadFileAsString() != "half and the rest"
            || root.getChildFile (".watched_with-sound.mov").exists())
            return fail ("the finished combined movie was not given its real name");
    }

    // The next take failing the same way is a second failure, not the first
    // one again. The app used to remember the problem's wording and announce
    // only a new one, so a second take whose combine failed for the same
    // reason said nothing at all.
    {
        if (! failingCombiner.start (root, failingPlan))
            return fail ("the second failing simulated combine did not start");

        const auto again = std::chrono::steady_clock::now() + std::chrono::seconds (2);
        while (failingCombiner.isRunning() && std::chrono::steady_clock::now() < again)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));

        const auto secondFailure = failingCombiner.getStatus();
        if (failingCombiner.isRunning() || secondFailure.problem != failedStatus.problem)
            return fail ("the second failing simulated combine did not fail the same way");

        if (secondFailure.run == 0 || secondFailure.run == failedStatus.run)
            return fail ("two takes' failed combines could not be told apart");
    }

    const auto successfulVideo = root.getChildFile ("success-input.mov");
    if (! successfulVideo.replaceWithText ("video"))
        return fail ("could not make the successful fake input");

    mma::CombinedTakePlan successfulPlan;
    successfulPlan.jobs.push_back ({ "success-input.mov", "MIX.wav",
                                     "successful-output.mov", 24, 0.0 });

    mma::TakeCombiner successfulCombiner;
    successfulCombiner.setFfmpegOverride (executable);

    if (! successfulCombiner.start (root, successfulPlan))
        return fail ("the successful simulated combine did not start");

    const auto successDeadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
    while (successfulCombiner.isRunning() && std::chrono::steady_clock::now() < successDeadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    if (successfulCombiner.isRunning())
        return fail ("the successful simulated combine did not finish");

    const auto successfulStatus = successfulCombiner.getStatus();
    if (successfulStatus.problem.isNotEmpty())
        return fail ("a zero ffmpeg exit was reported as a failure");

    if (! successfulStatus.written.contains ("successful-output.mov"))
        return fail ("a zero ffmpeg exit was not added to written outputs");

    if (! root.getChildFile ("successful-output.mov").existsAsFile())
        return fail ("a zero ffmpeg exit lost its completed output");

    // A take stopped while the previous take is still being combined must get
    // its combined file too. start() used to refuse it while a run was in
    // flight, the app ignored the refusal, and nothing retried: the second
    // take's "_with-sound" file silently never appeared.
    const auto secondTake = root.getChildFile ("second-take");

    if (! root.getChildFile ("slow-input.mov").replaceWithText ("video")
        || ! secondTake.createDirectory().wasOk()
        || ! secondTake.getChildFile ("success-input.mov").replaceWithText ("video")
        || ! secondTake.getChildFile ("MIX.wav").replaceWithText ("audio"))
        return fail ("could not make the two takes' fake inputs");

    mma::CombinedTakePlan firstTakePlan;
    firstTakePlan.jobs.push_back ({ "slow-input.mov", "MIX.wav",
                                    "first-take-output.mov", 24, 0.0 });

    mma::CombinedTakePlan secondTakePlan;
    secondTakePlan.jobs.push_back ({ "success-input.mov", "MIX.wav",
                                     "second-take-output.mov", 24, 0.0 });

    mma::TakeCombiner queueingCombiner;
    queueingCombiner.setFfmpegOverride (executable);

    if (! queueingCombiner.start (root, firstTakePlan) || ! queueingCombiner.isRunning())
        return fail ("the first take's simulated combine did not start");

    if (! queueingCombiner.start (secondTake, secondTakePlan))
        return fail ("a take stopped during another take's combine was turned away");

    const auto queueDeadline = std::chrono::steady_clock::now() + std::chrono::seconds (3);
    while (queueingCombiner.isRunning() && std::chrono::steady_clock::now() < queueDeadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    if (queueingCombiner.isRunning())
        return fail ("the queued simulated combine did not finish");

    if (! root.getChildFile ("first-take-output.mov").existsAsFile())
        return fail ("queueing a second take cost the first take its combined file");

    if (! secondTake.getChildFile ("second-take-output.mov").existsAsFile())
        return fail ("a take stopped during another take's combine never got its combined file");

    const auto queuedStatus = queueingCombiner.getStatus();
    if (queuedStatus.problem.isNotEmpty())
        return fail ("two clean queued combines were reported as a failure");

    if (queuedStatus.total != 2 || queuedStatus.done != 2
        || ! queuedStatus.written.contains ("first-take-output.mov")
        || ! queuedStatus.written.contains ("second-take-output.mov"))
        return fail ("the status did not account for both queued takes");

    // A take started while a combine is running gets the card: the combine's
    // ffmpeg is stopped and its file deleted, nothing is reported as failed,
    // and the combine runs again, whole, once the take is over.
    {
        const auto heldTake = root.getChildFile ("held-take");

        if (! heldTake.createDirectory().wasOk()
            || ! heldTake.getChildFile ("slow-input.mov").replaceWithText ("video")
            || ! heldTake.getChildFile ("MIX.wav").replaceWithText ("audio"))
            return fail ("could not make the held take's fake inputs");

        mma::CombinedTakePlan heldPlan;
        heldPlan.jobs.push_back ({ "slow-input.mov", "MIX.wav", "held-output.mov", 24, 0.0 });

        mma::TakeCombiner heldCombiner;
        heldCombiner.setFfmpegOverride (executable);

        if (! heldCombiner.start (heldTake, heldPlan))
            return fail ("the held take's simulated combine did not start");

        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        heldCombiner.setHeldForRecording (true); // Record pressed

        // Well past the 400 ms the stand-in takes to write its file.
        std::this_thread::sleep_for (std::chrono::milliseconds (700));

        if (heldTake.getChildFile ("held-output.mov").exists())
            return fail ("the combine kept writing while the next take recorded");

        if (! heldCombiner.isRunning() || heldCombiner.getStatus().problem.isNotEmpty())
            return fail ("a combine held for a take was reported as finished or failed");

        heldCombiner.setHeldForRecording (false); // the take stopped

        const auto heldDeadline = std::chrono::steady_clock::now() + std::chrono::seconds (3);
        while (heldCombiner.isRunning() && std::chrono::steady_clock::now() < heldDeadline)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));

        const auto heldStatus = heldCombiner.getStatus();

        if (heldCombiner.isRunning() || heldStatus.problem.isNotEmpty()
            || ! heldStatus.written.contains ("held-output.mov")
            || ! heldTake.getChildFile ("held-output.mov").existsAsFile())
            return fail ("the combine held for a take was not made once the take stopped");
    }

    // Only a Mac or Linux can hold the folder itself to follow it
    // (TakeFolderTracker); a Windows folder with open files can't be renamed.
   #if defined (__APPLE__) || defined (__linux__)
    // A take whose folder is renamed in Finder while its combine waits behind
    // the last take's. The combine looks for the take where it is now; it
    // used to list the old folder, find nothing there, and fail.
    {
        const auto queuedTake = root.getChildFile ("2026-10-11_2300_Queued");
        const auto renamedTake = root.getChildFile ("Queued (keep)");

        if (! root.getChildFile ("slow-input.mov").replaceWithText ("video")
            || ! queuedTake.createDirectory().wasOk()
            || ! queuedTake.getChildFile ("success-input.mov").replaceWithText ("video")
            || ! queuedTake.getChildFile ("MIX.wav").replaceWithText ("audio"))
            return fail ("could not make the renamed take's fake inputs");

        mma::CombinedTakePlan slowPlan;
        slowPlan.jobs.push_back ({ "slow-input.mov", "MIX.wav", "ahead-output.mov", 24, 0.0 });

        mma::CombinedTakePlan queuedPlan;
        queuedPlan.jobs.push_back ({ "success-input.mov", "MIX.wav", "queued-output.mov", 24, 0.0 });

        auto followed = std::make_shared<mma::TakeFolderTracker>();
        if (! followed->open (queuedTake.getFullPathName().toStdString()))
            return fail ("could not hold the renamed take's folder");

        mma::TakeCombiner renameCombiner;
        renameCombiner.setFfmpegOverride (executable);

        if (! renameCombiner.start (root, slowPlan)
            || ! renameCombiner.start (queuedTake, queuedPlan, std::move (followed)))
            return fail ("the renamed take's combine was not queued");

        // While the take ahead is still combining (its stand-in takes 400 ms).
        if (! queuedTake.moveFileTo (renamedTake))
            return fail ("could not rename the queued take's folder");

        const auto renameDeadline = std::chrono::steady_clock::now() + std::chrono::seconds (3);
        while (renameCombiner.isRunning() && std::chrono::steady_clock::now() < renameDeadline)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));

        const auto renameStatus = renameCombiner.getStatus();

        if (renameCombiner.isRunning() || renameStatus.problem.isNotEmpty()
            || ! renamedTake.getChildFile ("queued-output.mov").existsAsFile())
            return fail ("a take renamed while its combine waited never got its combined file");
    }
   #endif

    // "Install ffmpeg and the next take will combine them" has to be true.
    // A failed lookup used to be cached for the life of the app, so ffmpeg
    // installed after the first take was never found until a restart.
    {
       #if JUCE_WINDOWS
        const auto installed = root.getChildFile ("later-installed").getChildFile ("ffmpeg.exe");
       #else
        const auto installed = root.getChildFile ("later-installed").getChildFile ("ffmpeg");
       #endif

       #if ! JUCE_WINDOWS
        // A file that exists but does not run as ffmpeg -- an Intel build on
        // an Apple-silicon Mac without Rosetta -- is not ffmpeg.
        {
            const auto broken = root.getChildFile ("broken-ffmpeg");
            if (! broken.replaceWithText ("#!/bin/sh\nexit 1\n") || ! broken.setExecutePermission (true))
                return fail ("could not make the broken stand-in");

            mma::TakeCombiner brokenCombiner;
            brokenCombiner.setFfmpegSearchPathsForTesting ({ broken.getFullPathName().toStdString() });

            if (brokenCombiner.findFfmpeg().isNotEmpty())
                return fail ("an ffmpeg that exists but does not run was accepted");
        }
       #endif

        mma::TakeCombiner laterCombiner;
        laterCombiner.setFfmpegSearchPathsForTesting ({ installed.getFullPathName().toStdString() });

        if (laterCombiner.findFfmpeg().isNotEmpty())
            return fail ("an ffmpeg that is not there yet was reported as found");

        if (! installed.getParentDirectory().createDirectory().wasOk()
            || ! juce::File (executable).copyFileTo (installed))
            return fail ("could not install the stand-in ffmpeg");

        // Windows has no execute bit (JUCE reports false there); the .exe
        // name is what makes it runnable.
       #if ! JUCE_WINDOWS
        if (! installed.setExecutePermission (true))
            return fail ("could not install the stand-in ffmpeg");
       #endif

        const auto laterTake = root.getChildFile ("later-take");
        if (! laterTake.createDirectory().wasOk()
            || ! laterTake.getChildFile ("success-input.mov").replaceWithText ("video")
            || ! laterTake.getChildFile ("MIX.wav").replaceWithText ("audio"))
            return fail ("could not make the later take's fake inputs");

        mma::CombinedTakePlan laterPlan;
        laterPlan.jobs.push_back ({ "success-input.mov", "MIX.wav",
                                    "later-take-output.mov", 24, 0.0 });

        if (! laterCombiner.start (laterTake, laterPlan))
            return fail ("ffmpeg installed after a failed lookup was never found");

        const auto laterDeadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
        while (laterCombiner.isRunning() && std::chrono::steady_clock::now() < laterDeadline)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));

        if (! laterTake.getChildFile ("later-take-output.mov").existsAsFile())
            return fail ("the take after ffmpeg was installed did not combine");

        if (laterCombiner.findFfmpeg() != installed.getFullPathName())
            return fail ("an ffmpeg that was found was not remembered");
    }

    if (checkSplitMixReachesTheCombinedFile (root) != 0)
        return 1;

    std::printf ("ALL CHECKS PASSED (35 checks, 0 failing)\n");
    return 0;
}
