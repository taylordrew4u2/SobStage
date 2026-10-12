#include "TestFramework.h"
#include "Core/SessionRecovery.h"
#include "Core/SessionWriter.h"
#include "Core/Utf8Path.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mma;

namespace {

std::string tmpPath (const char* name)
{
    // Windows has no /tmp, so fall back through the usual temp-dir variables
    // before assuming a POSIX layout -- the same reasoning, and the same list,
    // as tempBasePath() in test_SessionWriter.cpp.
    for (const char* var : { "MMA_TEST_TMPDIR", "TMPDIR", "TMP", "TEMP" })
    {
        const char* dir = std::getenv (var);

        if (dir != nullptr && *dir != '\0')
            return std::string (dir) + "/mma-recovery-" + name;
    }

    return std::string ("/tmp/mma-recovery-") + name;
}

/// Writes a real take with SessionWriter -- the same code that writes a real
/// recording -- then truncates the header's size fields back to what they would
/// have said at the last five-second rewrite before a crash.
std::string writeTakeThenStaleTheHeader (const char* name, double seconds, bool staleHeader)
{
    const auto base = tmpPath (name);
    const auto path = base + ".wav";
    std::remove (path.c_str());

    SessionWriter writer;
    REQUIRE (writer.open (base, 48000.0, 1, 24, "2026-08-31T05:00:00Z"));

    const int frames = static_cast<int> (48000.0 * seconds);
    std::vector<float> block (static_cast<size_t> (frames), 0.25f);
    REQUIRE (writer.writeInterleaved (block.data(), static_cast<size_t> (frames)));
    writer.close(); // a clean stop, which writes correct sizes

    if (staleHeader)
    {
        // Put the file back into the state a killed process leaves it in: the
        // audio is all there, the header still describes the file as it was at
        // the last rewrite. Zero is the extreme of that -- the placeholder the
        // writer starts with, which is what a crash in the first five seconds
        // leaves behind.
        std::fstream f (path, std::ios::in | std::ios::out | std::ios::binary);
        const char zero[4] = { 0, 0, 0, 0 };
        f.seekp (4);            // RIFF size
        f.write (zero, 4);
        f.close();

        // And the data size, wherever it landed after the bext chunk: found by
        // searching for the tag rather than assuming an offset.
        std::fstream g (path, std::ios::in | std::ios::out | std::ios::binary);
        std::string all ((std::istreambuf_iterator<char> (g)), std::istreambuf_iterator<char>());
        const auto at = all.find ("data");
        REQUIRE (at != std::string::npos);
        g.clear();
        g.seekp (static_cast<std::streamoff> (at) + 4);
        g.write (zero, 4);
        g.close();
    }

    return path;
}

uint32_t readU32At (const std::string& path, std::streamoff at)
{
    std::ifstream f (path, std::ios::binary);
    f.seekg (at);
    unsigned char b[4] = {};
    f.read (reinterpret_cast<char*> (b), 4);
    return static_cast<uint32_t> (b[0]) | (static_cast<uint32_t> (b[1]) << 8)
         | (static_cast<uint32_t> (b[2]) << 16) | (static_cast<uint32_t> (b[3]) << 24);
}

} // namespace

TEST_CASE (SessionRecovery_aTakeWithNoStopTimestampIsInterrupted)
{
    SessionMetadata meta;
    meta.startTimestampIso = "2026-08-31T05:00:00Z";
    REQUIRE (SessionRecovery::sessionWasInterrupted (meta));

    meta.stopTimestampIso = "2026-08-31T05:04:00Z";
    REQUIRE_FALSE (SessionRecovery::sessionWasInterrupted (meta));
}

TEST_CASE (SessionRecovery_aStaleHeaderIsRepairedFromTheBytesOnDisk)
{
    const auto path = writeTakeThenStaleTheHeader ("stale", 3.0, true);

    const auto result = SessionRecovery::repairWavFile (path);

    // The audio was always there; only the header disagreed. §6.6's five-second
    // rewrite exists to make exactly this recoverable.
    REQUIRE (result.headerWasStale);
    REQUIRE (result.frames == 48000u * 3u);
    REQUIRE (result.seconds > 2.99);
    REQUIRE (result.seconds < 3.01);
    REQUIRE_FALSE (result.reportedEmpty);

    // And it was actually written back, not merely reported.
    REQUIRE (readU32At (path, 4) != 0u);

    // Running it again finds nothing left to do, which is what makes it safe to
    // run over a whole card at launch.
    const auto second = SessionRecovery::repairWavFile (path);
    REQUIRE_FALSE (second.headerWasStale);
    REQUIRE (second.frames == result.frames);

    std::remove (path.c_str());
}

TEST_CASE (SessionRecovery_aCleanlyClosedFileIsLeftAlone)
{
    const auto path = writeTakeThenStaleTheHeader ("clean", 2.0, false);

    const auto result = SessionRecovery::repairWavFile (path);

    REQUIRE_FALSE (result.headerWasStale);
    REQUIRE (result.frames == 48000u * 2u);
    REQUIRE_FALSE (result.reportedEmpty);

    std::remove (path.c_str());
}

TEST_CASE (SessionRecovery_underASecondIsReportedEmptyRatherThanOffered)
{
    // §6.6: "discard any recovered file containing under 1 second of audio;
    // report it as empty rather than presenting an unplayable stub."
    const auto path = writeTakeThenStaleTheHeader ("stub", 0.5, true);

    const auto result = SessionRecovery::repairWavFile (path);

    REQUIRE (result.reportedEmpty);
    REQUIRE (result.frames == 24000u);

    // Left on disk rather than deleted: removing something from a user's card
    // at launch, unasked, is a worse mistake than listing a short file.
    std::ifstream still (path, std::ios::binary);
    REQUIRE (still.good());
    still.close();

    std::remove (path.c_str());
}

TEST_CASE (SessionRecovery_rubbishIsReportedEmptyRatherThanThrowing)
{
    // A folder pulled off a card can contain anything at all, and launch is the
    // worst possible moment to throw.
    const auto path = tmpPath ("rubbish.wav");
    {
        std::ofstream f (path, std::ios::binary);
        f << "this is not a wav file, not even slightly";
    }

    const auto result = SessionRecovery::repairWavFile (path);
    REQUIRE (result.frames == 0u);
    REQUIRE (result.reportedEmpty);
    REQUIRE (result.fileName == std::string ("mma-recovery-rubbish.wav"));
    REQUIRE (result.fileName.find ('/') == std::string::npos);

    std::remove (path.c_str());

    // A path that is not there at all is the same answer, not a crash.
    const auto missing = SessionRecovery::repairWavFile (tmpPath ("no-such-file.wav"));
    REQUIRE (missing.frames == 0u);
    REQUIRE (missing.reportedEmpty);
}

TEST_CASE (SessionRecovery_aSessionCountsWhatIsWorthOffering)
{
    RecoveredSession session;
    session.folder = "/RECORDINGS/2026-08-31_0500_Session";
    session.files.push_back ({ "MIX.wav", 48000 * 4, 4.0, true, false });
    session.files.push_back ({ "01_Alice.wav", 48000 * 4, 4.0, true, false });
    session.files.push_back ({ "02_Bob.wav", 100, 0.002, true, true });

    REQUIRE (session.keptFileCount() == 2);
    REQUIRE (session.emptyFileCount() == 1);
    REQUIRE (session.longestSeconds() == 4.0);
    REQUIRE (session.isWorthPresenting());

    // A take where nothing survived is not presented at all: §6.6 would rather
    // say nothing than hand someone an unplayable stub.
    RecoveredSession allEmpty;
    allEmpty.files.push_back ({ "MIX.wav", 10, 0.0002, true, true });
    REQUIRE_FALSE (allEmpty.isWorthPresenting());
    REQUIRE (allEmpty.longestSeconds() == 0.0);
}

TEST_CASE (SessionRecovery_AFileThatIsThereButUnreadableIsNotCalledEmpty)
{
    // A path with nothing at it is nothing, and "empty" is the right answer --
    // that is the case above. A file that IS there and cannot be opened for
    // repair is a recording of unknown length on a card that has gone
    // read-only, and calling THAT empty sends the user away from audio that may
    // be perfectly intact. The two used to give the same answer.
    //
    // Driven through the read-only case by pointing at a directory: it exists,
    // so the existence check passes, and it cannot be opened as a file.
    // std::filesystem rather than mkdir: these tests run on Windows CI too.
    const auto dir = tmpPath ("mma-recovery-dir-case");
    std::filesystem::create_directory (dir);

    const auto result = SessionRecovery::repairWavFile (dir);

    REQUIRE_FALSE (result.reportedEmpty);
    REQUIRE (result.repairFailed);

    std::filesystem::remove (dir);
}

TEST_CASE (SessionRecovery_MalformedChunksCannotLoopOrReadOutsideTheirBounds)
{
    // Includes the exact overflow that used to seek back to the fmt header.
    for (const uint32_t size : { 0xfffffff8u, 0xffffffffu, 0u, 15u, 100u })
    {
        const auto path = tmpPath ("bad-chunk.wav");
        std::ofstream f (path, std::ios::binary);
        const auto u32 = [&f] (uint32_t value) {
            for (int i = 0; i < 4; ++i) f.put (static_cast<char> (value >> (8 * i)));
        };
        f.write ("RIFF", 4); u32 (28); f.write ("WAVEfmt ", 8); u32 (size);
        const char body[16] = { 1, 0, 1, 0, char(0x80), char(0xbb), 0, 0,
                               0, char(0x77), 1, 0, 2, 0, 16, 0 };
        f.write (body, 16); f.close();
        const auto before = std::filesystem::file_size (path);
        const auto result = SessionRecovery::repairWavFile (path);
        REQUIRE (result.frames == 0);
        REQUIRE_FALSE (result.headerWasStale);
        REQUIRE (std::filesystem::file_size (path) == before);
        std::remove (path.c_str());
    }
}

TEST_CASE (SessionRecovery_OddExtendedFmtAndUnknownChunksKeepTheirPadding)
{
    const auto path = tmpPath ("padded-chunks.wav");
    std::ofstream f (path, std::ios::binary);
    const auto u32 = [&f] (uint32_t value) {
        for (int i = 0; i < 4; ++i) f.put (static_cast<char> (value >> (8 * i)));
    };
    f.write ("RIFF", 4); u32 (0); f.write ("WAVEJUNK", 8); u32 (1);
    f.put ('x'); f.put (0);
    f.write ("fmt ", 4); u32 (17);
    const char body[16] = { 1, 0, 1, 0, 2, 0, 0, 0, 4, 0, 0, 0, 2, 0, 16, 0 };
    f.write (body, 16); f.put (0); f.put (0);
    f.write ("data", 4); u32 (0); f.write ("abcd", 4); f.close();
    const auto result = SessionRecovery::repairWavFile (path);
    REQUIRE (result.frames == 2);
    REQUIRE (result.headerWasStale);
    REQUIRE_FALSE (result.reportedEmpty);
    std::remove (path.c_str());
}

TEST_CASE (RecoveredSession_APlayableCountExcludesWhatCouldNotBeRepaired)
{
    // The recovery headline announced session.files.size() -- every file in the
    // folder -- as "repaired and can be played", directly under a warning
    // saying a particular file could not be opened. keptFileCount() is no help
    // either: a failed repair deliberately sets reportedEmpty = false, so it
    // counts as kept.
    RecoveredSession s;

    RecoveredFile good;
    good.fileName = "01_Alex.wav";

    RecoveredFile empty;
    empty.fileName = "02_Sam.wav";
    empty.reportedEmpty = true;

    RecoveredFile broken;
    broken.fileName = "03_Jo.wav";
    broken.repairFailed = true;   // reportedEmpty stays false, deliberately

    s.files = { good, empty, broken };

    REQUIRE (s.files.size() == 3);
    REQUIRE (s.keptFileCount() == 2);      // the broken one still counts here
    REQUIRE (s.playableFileCount() == 1);  // and must not count here
    REQUIRE (s.emptyFileCount() == 1);
}

TEST_CASE (Utf8Path_aNonAsciiFolderNameSurvivesTheRoundTrip)
{
    // Every path in Core is a UTF-8 std::string. Handed straight to fstream or
    // std::filesystem, MSVC reads it in the ANSI code page, so "Zo\xC3\xAB"
    // becomes "ZoÃ«" -- a different folder, which does not exist. The helper is
    // the one place that says "these bytes are UTF-8".
    const std::string name = "Zo\xC3\xAB";
    REQUIRE (utf8FromPath (pathFromUtf8 (name)) == name);
}

TEST_CASE (SessionRecovery_aTakeUnderANonAsciiFolderRecordsAndRepairs)
{
    // A user profile or card label with an accent in it -- C:\Users\Zoë --
    // made every take fail to start on Windows: the writer could not create
    // its first file, and recovery could not open what was there. Passes on
    // Linux either way (narrow paths are already UTF-8 there); on the Windows
    // CI job it fails without the u8path conversions in SessionWriter and
    // SessionRecovery.
    const auto folder = tmpPath ("Zo\xC3\xAB-take");
    std::error_code ec;
    std::filesystem::remove_all (pathFromUtf8 (folder), ec);
    REQUIRE (std::filesystem::create_directories (pathFromUtf8 (folder), ec));

    const auto base = folder + "/01_Zo\xC3\xAB";
    const auto path = base + ".wav";

    {
        SessionWriter writer;
        REQUIRE (writer.open (base, 48000.0, 1, 24, "2026-08-31T05:00:00Z"));

        std::vector<float> block (48000u * 2u, 0.25f);
        REQUIRE (writer.writeInterleaved (block.data(), block.size()));
        writer.close();
    }

    // The file landed under the name it was given, not a code-page mangling
    // of it beside the folder.
    REQUIRE (std::filesystem::exists (pathFromUtf8 (path), ec));

    {
        // Stale both size fields, as a killed process would leave them.
        std::fstream f (pathFromUtf8 (path), std::ios::in | std::ios::out | std::ios::binary);
        std::string all ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
        const auto at = all.find ("data");
        REQUIRE (at != std::string::npos);
        const char zero[4] = { 0, 0, 0, 0 };
        f.clear();
        f.seekp (4);
        f.write (zero, 4);
        f.seekp (static_cast<std::streamoff> (at) + 4);
        f.write (zero, 4);
    }

    const auto result = SessionRecovery::repairWavFile (path);
    REQUIRE (result.headerWasStale);
    REQUIRE_FALSE (result.repairFailed);
    REQUIRE_FALSE (result.reportedEmpty);
    REQUIRE (result.frames == 48000u * 2u);
    REQUIRE (result.fileName == "01_Zo\xC3\xAB.wav");

    std::filesystem::remove_all (pathFromUtf8 (folder), ec);
}

TEST_CASE (SessionRecovery_AHiddenBackupCopyIsRememberedSoItCanBeDismissedToo)
{
    const auto take = [] (const std::string& folder)
    {
        RecoveredSession s;
        s.folder = folder;
        s.files.push_back ({ "MIX.wav", 48000 * 4, 4.0, true, false });
        return s;
    };

    // Either scan can finish first; the card copy wins both ways, and the
    // backup's folder is kept so dismissing the card stamps it as well.
    for (const bool mirrorFirst : { true, false })
    {
        RecoveredSessionList list;
        const std::vector<RecoveredSession> mirror { take ("/Users/me/RECORDINGS-MIRROR/2026-09-01_2000_Take"),
                                                     take ("/Users/me/RECORDINGS-MIRROR/2026-09-01_2100_Other") };
        const std::vector<RecoveredSession> card { take ("/Volumes/CARD/RECORDINGS/2026-09-01_2000_Take") };

        if (mirrorFirst)
            list = SessionRecovery::mergeScan (SessionRecovery::mergeScan (std::move (list), mirror, false),
                                               card, true);
        else
            list = SessionRecovery::mergeScan (SessionRecovery::mergeScan (std::move (list), card, true),
                                               mirror, false);

        REQUIRE (list.shown.size() == 2);
        const auto shownTake = std::find_if (list.shown.begin(), list.shown.end(),
                                             [] (const RecoveredSession& s)
                                             { return s.folder.find ("2000_Take") != std::string::npos; });
        REQUIRE (shownTake != list.shown.end());
        REQUIRE (shownTake->folder == "/Volumes/CARD/RECORDINGS/2026-09-01_2000_Take");
        REQUIRE (list.hiddenFolders.size() == 1);
        REQUIRE (list.hiddenFolders[0] == "/Users/me/RECORDINGS-MIRROR/2026-09-01_2000_Take");
    }
}

TEST_CASE (SessionRecovery_anOddLengthDataChunkIsRepairedWithItsPadByte)
{
    // Mono 24-bit is three bytes a frame, so an odd number of frames is an odd
    // data chunk -- and RIFF chunks are word-aligned, so it needs a pad byte
    // after it that the RIFF size counts. Built by hand as the placeholder a
    // crash in the first five seconds leaves: both sizes still zero.
    const auto path = tmpPath ("odd-data.wav");
    std::remove (path.c_str());

    {
        std::ofstream f (path, std::ios::binary);
        const auto u32 = [&f] (uint32_t v)
        {
            const char b[4] = { static_cast<char> (v & 0xFF), static_cast<char> ((v >> 8) & 0xFF),
                                static_cast<char> ((v >> 16) & 0xFF), static_cast<char> ((v >> 24) & 0xFF) };
            f.write (b, 4);
        };
        const auto u16 = [&f] (uint16_t v)
        {
            const char b[2] = { static_cast<char> (v & 0xFF), static_cast<char> ((v >> 8) & 0xFF) };
            f.write (b, 2);
        };

        f.write ("RIFF", 4); u32 (0); f.write ("WAVE", 4);
        f.write ("fmt ", 4); u32 (16);
        u16 (1); u16 (1); u32 (48000); u32 (48000 * 3); u16 (3); u16 (24);
        f.write ("data", 4); u32 (0);
        const char frames[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        f.write (frames, 9);
    }

    constexpr std::streamoff dataSizeAt = 40;

    const auto result = SessionRecovery::repairWavFile (path);
    REQUIRE (result.headerWasStale);
    REQUIRE_FALSE (result.repairFailed);
    REQUIRE (result.frames == 3u);

    const auto size = static_cast<uint32_t> (std::filesystem::file_size (path));
    REQUIRE (size % 2 == 0);
    REQUIRE (readU32At (path, 4) == size - 8);
    REQUIRE (readU32At (path, dataSizeAt) == 9u);

    // Still safe to run twice: the pad byte is not mistaken for audio.
    const auto second = SessionRecovery::repairWavFile (path);
    REQUIRE_FALSE (second.headerWasStale);
    REQUIRE (second.frames == 3u);
    REQUIRE (std::filesystem::file_size (path) == size);

    std::remove (path.c_str());
}

TEST_CASE (SessionRecovery_theCardDoesNotCallUnrepairedFilesPlayable)
{
    // A card that went read-only took the repair on one file and dropped it on
    // the other. The card used to say the take "has been repaired and is
    // playable" either way.
    RecoveredSession session;
    session.folder = "/Volumes/CARD/RECORDINGS/2026-09-01_2000_Take";
    session.files.push_back ({ "MIX.wav", 48000 * 4, 4.0, true, false, false });
    session.files.push_back ({ "VOX.wav", 48000 * 4, 4.0, true, false, true });

    const auto row = recoveredTakeRow (session);
    REQUIRE (row.folderName == "2026-09-01_2000_Take");
    REQUIRE (row.fileCount == 2);
    REQUIRE (row.playableFileCount == 1);

    const auto explanation = recoveredTakesExplanation ({ row });
    REQUIRE (explanation.find ("is playable") == std::string::npos);
    REQUIRE (explanation.find ("couldn't be repaired") != std::string::npos);
    REQUIRE (recoveredTakeDetail (row) == "2 files, 4s of sound, 1 couldn't be repaired");

    // A take the repair fully reached still says so.
    session.files[1].repairFailed = false;
    const auto good = recoveredTakeRow (session);
    REQUIRE (recoveredTakesExplanation ({ good }).find ("repaired and is playable") != std::string::npos);
    REQUIRE (recoveredTakeDetail (good) == "2 files, 4s of sound");
}

TEST_CASE (SessionRecovery_aBackupFolderThatTookASuffixIsStillTheSameTake)
{
    // A backup folder of that name already existed -- the take restarted on a
    // new card after the first was pulled -- so the backup went to "_2". Matched
    // by name alone, one interrupted take was listed as two.
    const auto take = [] (const std::string& folder, const std::string& mirror)
    {
        RecoveredSession s;
        s.folder = folder;
        s.mirrorFolder = mirror;
        s.files.push_back ({ "MIX.wav", 48000 * 4, 4.0, true, false });
        return s;
    };

    for (const bool mirrorFirst : { true, false })
    {
        RecoveredSessionList list;
        const std::vector<RecoveredSession> mirror {
            take ("/Users/me/RECORDINGS-MIRROR/2026-09-01_2000_Take_2", "") };
        const std::vector<RecoveredSession> card {
            take ("/Volumes/CARD/RECORDINGS/2026-09-01_2000_Take",
                  "/Users/me/RECORDINGS-MIRROR/2026-09-01_2000_Take_2") };

        if (mirrorFirst)
            list = SessionRecovery::mergeScan (SessionRecovery::mergeScan (std::move (list), mirror, false),
                                               card, true);
        else
            list = SessionRecovery::mergeScan (SessionRecovery::mergeScan (std::move (list), card, true),
                                               mirror, false);

        REQUIRE (list.shown.size() == 1);
        REQUIRE (list.shown[0].folder == "/Volumes/CARD/RECORDINGS/2026-09-01_2000_Take");
        REQUIRE (list.hiddenFolders.size() == 1);
        REQUIRE (list.hiddenFolders[0] == "/Users/me/RECORDINGS-MIRROR/2026-09-01_2000_Take_2");
    }
}

TEST_CASE (SessionRecovery_theCardListsTheNewestTakeFirstWhicheverScanFinishedFirst)
{
    // The card's button is "Open the newest" and opens the first row. The
    // backup scan's older take used to come first just because that scan was
    // merged first.
    const auto take = [] (const std::string& folder, int64_t modifiedMs)
    {
        RecoveredSession s;
        s.folder = folder;
        s.modifiedMs = modifiedMs;
        s.files.push_back ({ "MIX.wav", 48000 * 4, 4.0, true, false });
        return s;
    };

    for (const bool mirrorFirst : { true, false })
    {
        RecoveredSessionList list;
        const std::vector<RecoveredSession> mirror { take ("/Users/me/RECORDINGS-MIRROR/2026-08-20_2000_Old", 1000) };
        const std::vector<RecoveredSession> card { take ("/Volumes/CARD/RECORDINGS/2026-09-01_2000_New", 5000),
                                                   take ("/Volumes/CARD/RECORDINGS/2026-08-01_2000_Older", 500) };

        if (mirrorFirst)
            list = SessionRecovery::mergeScan (SessionRecovery::mergeScan (std::move (list), mirror, false),
                                               card, true);
        else
            list = SessionRecovery::mergeScan (SessionRecovery::mergeScan (std::move (list), card, true),
                                               mirror, false);

        REQUIRE (list.shown.size() == 3);
        REQUIRE (list.shown[0].folder == "/Volumes/CARD/RECORDINGS/2026-09-01_2000_New");
        REQUIRE (list.shown[1].folder == "/Users/me/RECORDINGS-MIRROR/2026-08-20_2000_Old");
        REQUIRE (list.shown[2].folder == "/Volumes/CARD/RECORDINGS/2026-08-01_2000_Older");
    }
}

TEST_CASE (SessionRecovery_aPlayableBackupIsShownWhenTheCardsCopyCouldNotBeRepaired)
{
    // A locked card refused the header repair; the backup on this computer
    // took it. The card copy used to win anyway, so "Open the folder" went to
    // files with broken headers while playable ones sat in the backup.
    const auto take = [] (const std::string& folder, bool repairFailed)
    {
        RecoveredSession s;
        s.folder = folder;
        s.files.push_back ({ "MIX.wav", 48000 * 4, 4.0, true, false, repairFailed });
        s.files.push_back ({ "01_Alice.wav", 48000 * 4, 4.0, true, false, repairFailed });
        return s;
    };

    for (const bool mirrorFirst : { true, false })
    {
        RecoveredSessionList list;
        const std::vector<RecoveredSession> mirror { take ("/Users/me/RECORDINGS-MIRROR/2026-09-01_2000_Take", false) };
        const std::vector<RecoveredSession> card { take ("/Volumes/CARD/RECORDINGS/2026-09-01_2000_Take", true) };

        if (mirrorFirst)
            list = SessionRecovery::mergeScan (SessionRecovery::mergeScan (std::move (list), mirror, false),
                                               card, true);
        else
            list = SessionRecovery::mergeScan (SessionRecovery::mergeScan (std::move (list), card, true),
                                               mirror, false);

        REQUIRE (list.shown.size() == 1);
        REQUIRE (list.shown[0].folder == "/Users/me/RECORDINGS-MIRROR/2026-09-01_2000_Take");
        REQUIRE (list.shown[0].shownBecauseCardCopyUnrepairable);
        // Still dismissed together.
        REQUIRE (list.hiddenFolders.size() == 1);
        REQUIRE (list.hiddenFolders[0] == "/Volumes/CARD/RECORDINGS/2026-09-01_2000_Take");

        const auto row = recoveredTakeRow (list.shown[0]);
        REQUIRE (recoveredTakeDetail (row)
                 == "2 files, 4s of sound; the card's copy couldn't be repaired, so this is the local backup copy");
        REQUIRE (recoveredTakesExplanation ({ row }).find ("repaired and is playable") != std::string::npos);
    }

    // Both broken, or both fine: the card's copy, as before.
    for (const bool broken : { true, false })
    {
        auto list = SessionRecovery::mergeScan ({}, { take ("/Users/me/RECORDINGS-MIRROR/T", broken) }, false);
        list = SessionRecovery::mergeScan (std::move (list), { take ("/Volumes/CARD/RECORDINGS/T", broken) }, true);
        REQUIRE (list.shown.size() == 1);
        REQUIRE (list.shown[0].folder == "/Volumes/CARD/RECORDINGS/T");
        REQUIRE_FALSE (list.shown[0].shownBecauseCardCopyUnrepairable);
    }
}

TEST_CASE (SessionRecovery_aLockedCardsTakeIsOfferedFromTheBackupWithItsMoviesAccountedFor)
{
    // A locked card will not even open its files for the repair, so nothing on
    // it can be measured; the backup folder took a "_2" and is tied to the card
    // copy only by session.json's mirrorPath. The movies are on the card --
    // they are never backed up -- and the row has to say so, since Open goes
    // to the backup folder where they are not.
    RecoveredSession card;
    card.folder = "/Volumes/CARD/RECORDINGS/2026-09-01_2000_Take";
    card.mirrorFolder = "/Users/me/RECORDINGS-MIRROR/2026-09-01_2000_Take_2";
    SessionRecovery::countCameraMovie (card, "V01_FaceTime.mov", ".mov");
    card.modifiedMs = 2000;
    card.files.push_back ({ "01_Alice.wav", 0, 0.0, false, false, true });
    card.files.push_back ({ "MIX.wav", 0, 0.0, false, false, true });

    RecoveredSession backup;
    backup.folder = card.mirrorFolder;
    backup.modifiedMs = 1000;
    backup.files.push_back ({ "01_Alice.wav", 48000 * 3, 3.0, true, false, false });
    backup.files.push_back ({ "MIX.wav", 48000 * 3, 3.0, true, false, false });

    for (const bool cardFirst : { true, false })
    {
        RecoveredSessionList list;
        if (cardFirst)
            list = SessionRecovery::mergeScan (SessionRecovery::mergeScan ({}, { card }, true), { backup }, false);
        else
            list = SessionRecovery::mergeScan (SessionRecovery::mergeScan ({}, { backup }, false), { card }, true);

        REQUIRE (list.shown.size() == 1);
        REQUIRE (list.shown[0].folder == backup.folder);
        REQUIRE (list.shown[0].shownBecauseCardCopyUnrepairable);
        REQUIRE (list.hiddenFolders == std::vector<std::string> { card.folder });

        const auto row = recoveredTakeRow (list.shown[0]);
        REQUIRE (row.fullPath == backup.folder);
        REQUIRE (row.playableFileCount == row.fileCount);
        REQUIRE (recoveredTakeDetail (row)
                 == "2 files, 3s of sound; the card's copy couldn't be repaired, so this is the "
                    "local backup copy; 1 camera movie on the card only, which may not open");
    }
}

TEST_CASE (SessionRecovery_aBackupMissingPartOfTheTakeNeverStandsInForTheCard)
{
    // The card refused the repair but its bytes were counted: 60 s of sound.
    const auto cardCopy = []
    {
        RecoveredSession s;
        s.folder = "/Volumes/CARD/RECORDINGS/T";
        s.files.push_back ({ "01_Alice.wav", 48000 * 60, 60.0, true, false, true });
        s.files.push_back ({ "MIX.wav", 48000 * 60, 60.0, true, false, true });
        return s;
    };
    const auto backupCopy = [] (double seconds)
    {
        RecoveredSession s;
        s.folder = "/Users/me/RECORDINGS-MIRROR/T";
        s.files.push_back ({ "01_Alice.wav", (uint64_t) (48000 * seconds), seconds, true, false, false });
        s.files.push_back ({ "MIX.wav", (uint64_t) (48000 * seconds), seconds, true, false, false });
        return s;
    };
    const auto shownFolder = [] (const RecoveredSession& card, const RecoveredSession& backup)
    {
        auto list = SessionRecovery::mergeScan ({}, { backup }, false);
        list = SessionRecovery::mergeScan (std::move (list), { card }, true);
        REQUIRE (list.shown.size() == 1);
        REQUIRE (list.hiddenFolders.size() == 1);
        return list.shown[0].folder;
    };

    // The whole take, give or take the last moment: the backup.
    REQUIRE (shownFolder (cardCopy(), backupCopy (60.0)) == "/Users/me/RECORDINGS-MIRROR/T");
    REQUIRE (shownFolder (cardCopy(), backupCopy (60.0 - SessionRecovery::kBackupLengthToleranceSeconds / 2))
             == "/Users/me/RECORDINGS-MIRROR/T");

    // The backup stopped forty seconds in -- its drive filled -- and repaired
    // cleanly. Offering it would cost the user the end of the take.
    REQUIRE (shownFolder (cardCopy(), backupCopy (20.0)) == "/Volumes/CARD/RECORDINGS/T");

    // Cut short, as the take's own record says: either copy's record will do,
    // because the card's may have been refreshed after the backup stopped and
    // the backup's could not be.
    for (const bool saidByCard : { true, false })
    {
        auto card = cardCopy();
        auto backup = backupCopy (60.0);
        (saidByCard ? card : backup).backupCopyCutShort = true;
        REQUIRE (shownFolder (card, backup) == "/Volumes/CARD/RECORDINGS/T");
    }

    // A file the card has and the backup does not.
    {
        auto backup = backupCopy (60.0);
        backup.files.pop_back();
        REQUIRE (shownFolder (cardCopy(), backup) == "/Volumes/CARD/RECORDINGS/T");
    }

    // A backup with a file of its own that would not repair is no better.
    {
        auto backup = backupCopy (60.0);
        backup.files.back().repairFailed = true;
        REQUIRE (shownFolder (cardCopy(), backup) == "/Volumes/CARD/RECORDINGS/T");
    }

    // And the card copy, when shown, still says nothing about a backup.
    {
        auto list = SessionRecovery::mergeScan ({}, { backupCopy (20.0) }, false);
        list = SessionRecovery::mergeScan (std::move (list), { cardCopy() }, true);
        const auto row = recoveredTakeRow (list.shown[0]);
        REQUIRE_FALSE (row.isBackupBecauseCardCopyUnrepairable);
        REQUIRE (recoveredTakeDetail (row).find ("backup") == std::string::npos);
    }
}

TEST_CASE (SessionRecovery_theCardSaysWhatACrashMeansForTheCameraMovies)
{
    // The movies beside the sound were never mentioned, and nothing repairs
    // them: an interrupted movie ends at its last ten-second fragment.
    RecoveredSession session;
    session.folder = "/Volumes/CARD/RECORDINGS/2026-09-01_2000_Take";
    session.files.push_back ({ "MIX.wav", 48000 * 64, 64.0, true, false, false });
    session.movieCount = 2;
    session.quickTimeMovieCount = 2;

    const auto row = recoveredTakeRow (session);
    REQUIRE (row.movieCount == 2);
    REQUIRE (recoveredTakeDetail (row) == "1 file, 1m 4s of sound; 2 camera movies, which may end up to 10 s early");

    // Shorter than one fragment: there may be nothing in the movie to open.
    session.files[0].seconds = 4.0;
    session.movieCount = 1;
    session.quickTimeMovieCount = 1;
    REQUIRE (recoveredTakeDetail (recoveredTakeRow (session))
             == "1 file, 4s of sound; 1 camera movie, which may not open");

    // No camera, no mention.
    session.movieCount = 0;
    session.quickTimeMovieCount = 0;
    REQUIRE (recoveredTakeDetail (recoveredTakeRow (session)) == "1 file, 4s of sound");
}

TEST_CASE (SessionRecovery_aWindowsTakesMoviesAreCountedWithoutAQuickTimePromise)
{
    // JUCE's Windows cameras write .wmv. The scan counted *.mov and *.mp4
    // only, so a Windows take's recovery row never mentioned its movies --
    // and AVFoundation's ten-second fragments are nothing a .wmv promises.
    RecoveredSession session;
    session.folder = "D:\\RECORDINGS\\2026-09-01_2000_Take";
    session.files.push_back ({ "MIX.wav", 48000 * 64, 64.0, true, false, false });

    for (const char* name : { "V01_Logitech.wmv", "V02_Capture.WMV", "._V01_Logitech.wmv", "MIX.wav",
                              "session.json", "activity.log", ".DS_Store", "notes.txt" })
        SessionRecovery::countCameraMovie (session, name, ".wmv");

    REQUIRE (session.movieCount == 2);
    REQUIRE (session.quickTimeMovieCount == 0);
    REQUIRE (recoveredTakeDetail (recoveredTakeRow (session))
             == "1 file, 1m 4s of sound; 2 camera movies, which weren't finished and may not open");

    session.files[0].seconds = 4.0;
    session.movieCount = 0;
    SessionRecovery::countCameraMovie (session, "V01_Logitech.wmv", ".wmv");
    REQUIRE (recoveredTakeDetail (recoveredTakeRow (session))
             == "1 file, 4s of sound; 1 camera movie, which wasn't finished and may not open");

    // A Mac's movies, read on Windows, are still QuickTime's; a card with
    // both kinds is promised nothing about either.
    RecoveredSession mixed = session;
    mixed.movieCount = 0;
    mixed.quickTimeMovieCount = 0;
    SessionRecovery::countCameraMovie (mixed, "V01_FaceTime.mov", ".wmv");
    REQUIRE (mixed.movieCount == 1);
    REQUIRE (mixed.quickTimeMovieCount == 1);
    REQUIRE (recoveredTakeDetail (recoveredTakeRow (mixed)) == "1 file, 4s of sound; 1 camera movie, which may not open");
    SessionRecovery::countCameraMovie (mixed, "V02_Logitech.wmv", ".mov");
    REQUIRE (recoveredTakeDetail (recoveredTakeRow (mixed))
             == "1 file, 4s of sound; 2 camera movies, which weren't finished and may not open");

    // Whatever container this computer's cameras write is a movie.
    RecoveredSession other;
    SessionRecovery::countCameraMovie (other, "V01_Cam.avi", ".avi");
    SessionRecovery::countCameraMovie (other, "V02_Cam.mp4", ".avi");
    SessionRecovery::countCameraMovie (other, "V03_Cam.avi", ".mov");
    REQUIRE (other.movieCount == 2);
    REQUIRE (other.quickTimeMovieCount == 0);
}

// A details file that could not be read is not an empty one. Both used to come
// back as an empty string, which the lenient parser turned into a record with
// no stop time: a finished take kept as an iCloud placeholder, opened offline,
// was called interrupted, and dismissing it renamed a near-empty record over
// the real one.
TEST_CASE (SessionRecovery_aDetailsFileThatCannotBeReadIsNotAnEmptyOne)
{
    namespace fs = std::filesystem;

    // Read: every byte, as written -- including ones a text read would bend.
    const auto written = tmpPath ("record-read.json");
    const std::string record = std::string ("{\"stopTimestampIso\":\"2026-10-11T22:00:00Z\"}\r\n")
                             + std::string (1, '\0') + "tail";
    {
        std::ofstream out (written, std::ios::binary | std::ios::trunc);
        out.write (record.data(), static_cast<std::streamsize> (record.size()));
    }
    const auto read = SessionRecovery::readWholeFileChecked (written);
    REQUIRE (read.has_value());
    REQUIRE (*read == record);

    // A file a power cut emptied WAS read: it holds nothing, and that is an
    // answer. The scan still treats it as interrupted and Done still stamps
    // it, or such a take would come back at every launch.
    const auto emptied = tmpPath ("record-empty.json");
    {
        std::ofstream out (emptied, std::ios::binary | std::ios::trunc);
    }
    const auto empty = SessionRecovery::readWholeFileChecked (emptied);
    REQUIRE (empty.has_value());
    REQUIRE (empty->empty());

    // Not there.
    REQUIRE (! SessionRecovery::readWholeFileChecked (tmpPath ("record-not-there.json")).has_value());

    // There, but its bytes will not come: a directory opens on POSIX and then
    // refuses every read.
    const auto unreadable = tmpPath ("record-unreadable.json");
    std::error_code ec;
    fs::remove_all (unreadable, ec);
    REQUIRE (fs::create_directory (unreadable));
    REQUIRE (! SessionRecovery::readWholeFileChecked (unreadable).has_value());

    fs::remove (written, ec);
    fs::remove (emptied, ec);
    fs::remove_all (unreadable, ec);
}

namespace {

/// Delivers `good` bytes, then fails the way a device read does.
class FailingAfter : public std::streambuf
{
public:
    explicit FailingAfter (std::string goodBytes) : good (std::move (goodBytes))
    {
        setg (good.data(), good.data(), good.data() + good.size());
    }

protected:
    int_type underflow() override { throw std::runtime_error ("read failed"); }

private:
    std::string good;
};

} // namespace

// What an offline cloud placeholder does: it reports its full size and then
// delivers less, or nothing. Fewer bytes than promised is "could not read".
TEST_CASE (SessionRecovery_fewerBytesThanTheFileSaysItHoldsIsNotARead)
{
    {
        std::istringstream whole ("{\"a\":1}");
        const auto bytes = SessionRecovery::readExactly (whole, 7);
        REQUIRE (bytes.has_value());
        REQUIRE (*bytes == "{\"a\":1}");
    }
    {
        std::istringstream nothing ("");
        REQUIRE (! SessionRecovery::readExactly (nothing, 4096).has_value());
    }
    {
        std::istringstream cutShort ("{\"a\":");
        REQUIRE (! SessionRecovery::readExactly (cutShort, 7).has_value());
    }
    {
        FailingAfter buffer ("{\"a\"");
        std::istream failing (&buffer);
        REQUIRE (! SessionRecovery::readExactly (failing, 7).has_value());
    }
    {
        std::istringstream emptyFile ("");
        const auto bytes = SessionRecovery::readExactly (emptyFile, 0);
        REQUIRE (bytes.has_value());
        REQUIRE (bytes->empty());
    }
}
