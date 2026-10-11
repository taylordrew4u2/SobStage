// TakeCombiner's Mac branch, run on any machine.
//
// On a Mac the combined video is made by AVFoundation (MacMovieCombiner.mm),
// which only builds there. What TakeCombiner does around it -- holding the
// copy while a take records, starting it again afterwards, checking the card
// has room -- is plain C++ and is the part that decides whether the next take
// gets the card to itself. This builds TakeCombiner.cpp exactly as the Mac app
// does (MMA_NATIVE_MOVIE_COMBINER) and links it against a stand-in for
// combineMovieWithSound that behaves like the real one: it writes a hidden
// working file beside the output, asks shouldStop() while it copies, deletes
// its working file when stopped, and renames it into place when finished.

#include "App/TakeCombiner.h"
#include "Platform/MacMovieCombiner.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

namespace {

int checks = 0;
int failures = 0;

void expect (bool condition, const char* what)
{
    ++checks;

    if (! condition)
    {
        ++failures;
        std::fprintf (stderr, "FAIL  %s\n", what);
    }
}

/// How long the stand-in's copy takes, and how often it was asked to make one.
std::atomic<int> exportMs { 600 };
std::atomic<int> exportsStarted { 0 };

/// Whether the stand-in fails, as AVFoundation can, when a part of the mix is
/// no longer where it was when the copy began.
std::atomic<bool> failsIfAPartMoved { false };

bool writeText (const std::filesystem::path& path, const char* text)
{
    std::ofstream out (path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool> (out);
}

using Clock = std::chrono::steady_clock;

template <typename Condition>
bool waitFor (Condition condition, int ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds (ms);
    while (! condition())
    {
        if (Clock::now() > deadline)
            return false;

        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    return true;
}

void sleepMs (int ms)
{
    std::this_thread::sleep_for (std::chrono::milliseconds (ms));
}

} // namespace

namespace mma {

std::string combineMovieWithSound (const std::string& videoPath,
                                   const std::vector<std::string>& audioParts,
                                   const std::string& outputPath,
                                   double,
                                   const std::function<bool()>& shouldStop,
                                   const std::function<std::string()>& currentFolder)
{
    ++exportsStarted;

    std::error_code ec;
    if (audioParts.empty() || ! std::filesystem::exists (videoPath, ec))
        return "The camera's file has no picture in it.";

    const std::filesystem::path output (outputPath);
    const auto folderAtStart = output.parent_path();
    const auto outputName = output.filename();
    const auto workingName = std::filesystem::path ("." + outputName.string());

    // Asked only once the export has ended, as the real one does.
    const auto folderNow = [&]
    {
        const auto now = currentFolder != nullptr ? currentFolder() : std::string();
        return now.empty() ? folderAtStart : std::filesystem::path (now);
    };

    std::filesystem::remove (output, ec);
    std::filesystem::remove (folderAtStart / workingName, ec);

    // Written through a file held open for the whole copy, as AVFoundation
    // writes its export: a folder renamed meanwhile takes the file with it.
    std::ofstream working (folderAtStart / workingName, std::ios::binary | std::ios::trunc);
    if (! working)
        return "macOS couldn't set up the combined file.";

    working << "partial";
    working.flush();

    // The real one polls every 100 ms; quicker here only to keep the run short.
    const auto finishAt = Clock::now() + std::chrono::milliseconds (exportMs.load());
    while (Clock::now() < finishAt)
    {
        if (shouldStop())
        {
            working.close();
            std::filesystem::remove (folderNow() / workingName, ec);
            return "Stopped before the combined file was finished.";
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    working << " complete";
    working.close();

    const auto folder = folderNow();

    // An export that reads a later part of the mix by name once the folder
    // has moved finds nothing there, and fails.
    if (failsIfAPartMoved.load())
        for (const auto& part : audioParts)
            if (! std::filesystem::exists (part, ec))
            {
                std::filesystem::remove (folder / workingName, ec);
                return "Couldn't read the sound in " + part + ".";
            }

    std::filesystem::rename (folder / workingName, folder / outputName, ec);
    if (ec)
    {
        std::filesystem::remove (folder / workingName, ec);
        return "Couldn't name the finished combined file: " + ec.message();
    }

    return {};
}

} // namespace mma

int main()
{
    const auto root = std::filesystem::temp_directory_path()
                    / ("sobstage-native-combine-" + std::to_string (Clock::now().time_since_epoch().count()));
    std::filesystem::create_directories (root);

    struct Cleanup
    {
        std::filesystem::path root;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all (root, ec); }
    } cleanup { root };

    auto makeTake = [&root] (const char* name)
    {
        const auto take = root / name;
        std::filesystem::create_directories (take);
        writeText (take / "V01_Cam.mov", "video");
        writeText (take / "MIX.wav", "audio");
        return take;
    };

    mma::CombinedTakePlan plan;
    plan.jobs.push_back ({ "V01_Cam.mov", "MIX.wav", "V01_Cam_with-sound.mov", 24, 0.0 });

    auto folderOf = [] (const std::filesystem::path& take)
    {
        return juce::File (juce::String (take.string()));
    };

    std::error_code ec;

    // 1. The Mac app's combine is the built-in one: nothing to install.
    {
        mma::TakeCombiner combiner;
        expect (combiner.findFfmpeg() == mma::TakeCombiner::kBuiltInCombiner,
                "the Mac build does not use its own combine");
    }

    // 2. A take started while the combine is copying: the copy stops at once
    //    and leaves nothing behind, waits for as long as the take records --
    //    however long that is -- and is made again, whole, after it. Nothing
    //    about that is a failure.
    {
        const auto take = makeTake ("held-mid-copy");
        exportsStarted = 0;
        exportMs = 600;

        mma::TakeCombiner combiner;
        expect (combiner.start (folderOf (take), plan), "the combine did not start");
        expect (waitFor ([] { return exportsStarted.load() == 1; }, 2000), "the copy never began");
        sleepMs (100);

        combiner.setHeldForRecording (true); // Record pressed
        sleepMs (250);

        expect (! std::filesystem::exists (take / ".V01_Cam_with-sound.mov", ec),
                "a copy stopped for a take left its working file on the card");
        expect (! std::filesystem::exists (take / "V01_Cam_with-sound.mov", ec),
                "a copy stopped for a take left a file named as the finished one");
        expect (combiner.isRunning(), "a held combine stopped counting as running (quit would not wait for it)");

        // Longer than the whole copy takes: still nothing, and not begun again.
        sleepMs (900);
        expect (exportsStarted.load() == 1, "the combine started again while the take was still recording");
        expect (! std::filesystem::exists (take / "V01_Cam_with-sound.mov", ec),
                "the combine wrote its file while the take was still recording");

        const auto heldStatus = combiner.getStatus();
        expect (heldStatus.problem.isEmpty() && heldStatus.done == 0 && heldStatus.running,
                "a held combine was reported as failed or finished");

        combiner.setHeldForRecording (false); // the take stopped
        expect (waitFor ([&combiner] { return ! combiner.isRunning(); }, 3000),
                "the held combine did not finish once the take stopped");

        const auto status = combiner.getStatus();
        expect (exportsStarted.load() == 2, "the interrupted copy was not made again from the start");
        expect (std::filesystem::exists (take / "V01_Cam_with-sound.mov", ec),
                "the take that interrupted the combine cost the last take its combined video");
        expect (! std::filesystem::exists (take / ".V01_Cam_with-sound.mov", ec),
                "the finished combine left its working file behind");
        expect (status.problem.isEmpty(), "a combine that waited for a take was reported as a problem");
        expect (status.done == 1 && status.total == 1 && status.written.contains ("V01_Cam_with-sound.mov"),
                "the status does not show the combined file that was made after the take");
    }

    // 3. A take stopped while the next one is already recording (or its
    //    movies are still being finished): its combine is queued, and not
    //    begun until the card is free.
    {
        const auto take = makeTake ("started-while-held");
        exportsStarted = 0;
        exportMs = 50;

        mma::TakeCombiner combiner;
        combiner.setHeldForRecording (true);
        expect (combiner.start (folderOf (take), plan), "a combine asked for during a take was refused");
        sleepMs (300);

        expect (exportsStarted.load() == 0, "a combine began while a take had the card");
        expect (combiner.isRunning(), "a combine waiting for a take did not count as running");

        combiner.setHeldForRecording (false);
        expect (waitFor ([&combiner] { return ! combiner.isRunning(); }, 3000),
                "a waiting combine did not run once the take was over");
        expect (std::filesystem::exists (take / "V01_Cam_with-sound.mov", ec)
                    && combiner.getStatus().problem.isEmpty(),
                "a waiting combine did not make its file once the take was over");
    }

    // 4. Quitting still wins over waiting: a held combine ends promptly, and
    //    makes nothing.
    {
        const auto take = makeTake ("cancelled-while-held");
        exportsStarted = 0;

        auto combiner = std::make_unique<mma::TakeCombiner>();
        combiner->setHeldForRecording (true);
        expect (combiner->start (folderOf (take), plan), "the held combine did not start");
        sleepMs (50);

        const auto before = Clock::now();
        combiner->cancel();
        expect (waitFor ([&combiner] { return ! combiner->isRunning(); }, 1000),
                "a held combine kept waiting after quitting cancelled it");
        expect (Clock::now() - before < std::chrono::milliseconds (500),
                "a held combine took too long to notice quitting");
        expect (exportsStarted.load() == 0 && ! std::filesystem::exists (take / "V01_Cam_with-sound.mov", ec),
                "a combine cancelled while held made a file");

        // And a cancelled combine is not woken into copying by a release.
        combiner->setHeldForRecording (false);
        sleepMs (100);
        expect (exportsStarted.load() == 0, "releasing a cancelled combine started it");
    }

    // 5. Not enough room on the card for the combined file: not attempted,
    //    the reason said, nothing written.
    {
        const auto take = makeTake ("no-room");
        exportsStarted = 0;
        exportMs = 20;

        mma::TakeCombiner combiner;
        combiner.setAvailableBytesForTesting ([] (const std::string&) -> std::optional<uint64_t>
        {
            return 512ull << 20; // half a gigabyte: less than the headroom alone
        });

        expect (combiner.start (folderOf (take), plan), "the combine on a full card did not start");
        expect (waitFor ([&combiner] { return ! combiner.isRunning(); }, 2000), "the full-card combine did not finish");

        const auto status = combiner.getStatus();
        std::printf ("  full card: %s\n", status.problem.toRawUTF8());
        expect (exportsStarted.load() == 0, "a combine was copied onto a card without room for it");
        expect (status.problem.contains ("room on the card"), "a combine skipped for room does not say so");
        expect (status.problem.contains ("separate picture and sound files are all still there"),
                "a combine skipped for room does not say the take is safe");
        expect (! std::filesystem::exists (take / "V01_Cam_with-sound.mov", ec),
                "a combine skipped for room wrote a file");
    }

    // 6. A card that cannot say how much room it has is tried, as before.
    {
        const auto take = makeTake ("room-unknown");
        exportsStarted = 0;
        exportMs = 20;

        mma::TakeCombiner combiner;
        combiner.setAvailableBytesForTesting ([] (const std::string&) -> std::optional<uint64_t>
        {
            return std::nullopt;
        });

        expect (combiner.start (folderOf (take), plan), "the combine did not start");
        expect (waitFor ([&combiner] { return ! combiner.isRunning(); }, 2000), "the combine did not finish");
        expect (exportsStarted.load() == 1 && std::filesystem::exists (take / "V01_Cam_with-sound.mov", ec),
                "a card that could not report its room was refused the combine");
    }

    // Only a Mac or Linux can hold the folder itself to follow it
    // (TakeFolderTracker); a Windows folder with open files can't be renamed.
   #if defined (__APPLE__) || defined (__linux__)
    // 7. The take folder renamed in Finder while the copy runs: the finished
    //    file is named in the folder as it is now. Looked for in the old
    //    place, the rename failed, the user was told the combine had, and the
    //    next launch deleted the finished file, still hidden, as unfinished.
    {
        const auto take = makeTake ("2026-10-11_2100_Show");
        const auto renamed = root / "Show (keep)";
        exportsStarted = 0;
        exportMs = 400;
        failsIfAPartMoved = false;

        auto followed = std::make_shared<mma::TakeFolderTracker>();
        expect (followed->open (take.string()), "could not hold the take folder");

        mma::TakeCombiner combiner;
        expect (combiner.start (folderOf (take), plan, std::move (followed)), "the combine did not start");
        expect (waitFor ([] { return exportsStarted.load() == 1; }, 2000), "the copy never began");
        sleepMs (50);

        std::filesystem::rename (take, renamed, ec);
        expect (! ec, "could not rename the take folder");

        expect (waitFor ([&combiner] { return ! combiner.isRunning(); }, 3000),
                "the combine in a renamed folder did not finish");

        const auto status = combiner.getStatus();
        if (status.problem.isNotEmpty())
            std::printf ("  renamed mid-copy: %s\n", status.problem.toRawUTF8());

        expect (status.problem.isEmpty() && status.written.contains ("V01_Cam_with-sound.mov"),
                "a take folder renamed during the combine was reported as a failed combine");
        expect (std::filesystem::exists (renamed / "V01_Cam_with-sound.mov", ec),
                "the combined file is not in the renamed take folder");
        expect (! std::filesystem::exists (renamed / ".V01_Cam_with-sound.mov", ec),
                "the renamed take folder was left holding the hidden working file");
        expect (exportsStarted.load() == 1, "a combine in a renamed folder was copied twice");
    }

    // 8. Renamed, then Record pressed mid-copy: the working file is cleared
    //    out of the renamed folder, and the copy made again there afterwards.
    {
        const auto take = makeTake ("2026-10-11_2130_Encore");
        const auto renamed = root / "Encore (keep)";
        exportsStarted = 0;
        exportMs = 600;
        failsIfAPartMoved = false;

        auto followed = std::make_shared<mma::TakeFolderTracker>();
        expect (followed->open (take.string()), "could not hold the encore's folder");

        mma::TakeCombiner combiner;
        expect (combiner.start (folderOf (take), plan, std::move (followed)), "the encore's combine did not start");
        expect (waitFor ([] { return exportsStarted.load() == 1; }, 2000), "the encore's copy never began");
        sleepMs (50);

        std::filesystem::rename (take, renamed, ec);
        expect (! ec, "could not rename the encore's folder");
        sleepMs (50);

        combiner.setHeldForRecording (true);
        sleepMs (250);
        expect (! std::filesystem::exists (renamed / ".V01_Cam_with-sound.mov", ec),
                "a copy stopped for a take left its working file in the renamed folder");

        combiner.setHeldForRecording (false);
        expect (waitFor ([&combiner] { return ! combiner.isRunning(); }, 3000),
                "the encore's combine did not finish after the take");
        expect (exportsStarted.load() == 2 && combiner.getStatus().problem.isEmpty()
                    && std::filesystem::exists (renamed / "V01_Cam_with-sound.mov", ec),
                "the copy stopped for a take was not made again in the renamed folder");
    }

    // 9. A copy that fails because the folder moved under it -- a later part
    //    of the mix asked for by its old name -- is made once more, from
    //    where the folder is now.
    {
        const auto take = makeTake ("2026-10-11_2200_Late");
        const auto renamed = root / "Late (keep)";
        exportsStarted = 0;
        exportMs = 300;
        failsIfAPartMoved = true;

        auto followed = std::make_shared<mma::TakeFolderTracker>();
        expect (followed->open (take.string()), "could not hold the late take's folder");

        mma::TakeCombiner combiner;
        expect (combiner.start (folderOf (take), plan, std::move (followed)), "the late take's combine did not start");
        expect (waitFor ([] { return exportsStarted.load() == 1; }, 2000), "the late take's copy never began");
        sleepMs (50);

        std::filesystem::rename (take, renamed, ec);
        expect (! ec, "could not rename the late take's folder");

        expect (waitFor ([&combiner] { return ! combiner.isRunning(); }, 3000),
                "the late take's combine did not finish");

        const auto status = combiner.getStatus();
        expect (exportsStarted.load() == 2, "a copy failed by a moved folder was not made again from its new place");
        expect (status.problem.isEmpty() && std::filesystem::exists (renamed / "V01_Cam_with-sound.mov", ec),
                "a copy failed by a moved folder did not end with the combined file");
        expect (! std::filesystem::exists (renamed / ".V01_Cam_with-sound.mov", ec),
                "a copy failed by a moved folder left its working file behind");

        failsIfAPartMoved = false;
    }
   #endif

    std::printf ("%s (%d checks, %d failing)\n",
                 failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED", checks, failures);
    return failures == 0 ? 0 : 1;
}
