#include "TestFramework.h"
#include "Core/SessionWriter.h"
#include <cstdio>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <filesystem>

using namespace mma;

namespace {

std::string tempBasePath (const std::string& name)
{
    // Windows has no /tmp, so fall back through the usual temp-dir variables
    // before assuming a POSIX layout.
    for (const char* var : { "MMA_TEST_TMPDIR", "TMPDIR", "TMP", "TEMP" })
    {
        const char* dir = std::getenv (var);

        if (dir != nullptr && *dir != '\0')
            return std::string (dir) + "/" + name;
    }

    return "/tmp/" + name;
}

uint32_t readU32LE (std::ifstream& f, std::streampos pos)
{
    f.seekg (pos);
    unsigned char b[4];
    f.read (reinterpret_cast<char*> (b), 4);
    return static_cast<uint32_t> (b[0]) | (static_cast<uint32_t> (b[1]) << 8)
         | (static_cast<uint32_t> (b[2]) << 16) | (static_cast<uint32_t> (b[3]) << 24);
}

uint64_t readU64LE (std::ifstream& f, std::streampos pos)
{
    f.seekg (pos);
    unsigned char bytes[8] {};
    f.read (reinterpret_cast<char*> (bytes), 8);

    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value |= static_cast<uint64_t> (bytes[i]) << (8 * i);
    return value;
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

TEST_CASE (SessionWriter_WritesReadableRiffWaveHeader)
{
    std::string path = tempBasePath ("mma_test_basic");
    SessionWriter writer;
    REQUIRE (writer.open (path, 48000.0, 2, 16, "2026-08-26T14:32:00Z"));

    std::vector<float> frames = { 0.5f, -0.5f, 0.25f, -0.25f };
    REQUIRE (writer.writeInterleaved (frames.data(), 2));
    writer.close();

    std::ifstream f (writer.getCurrentFilePath(), std::ios::binary);
    REQUIRE (f.is_open());
    char riffTag[4]; f.read (riffTag, 4);
    REQUIRE (std::string (riffTag, 4) == "RIFF");
    char waveTag[4]; f.seekg (8); f.read (waveTag, 4);
    REQUIRE (std::string (waveTag, 4) == "WAVE");

    std::remove (writer.getCurrentFilePath().c_str());
}

TEST_CASE (SessionWriter_DataChunkSizeMatchesBytesWritten)
{
    std::string path = tempBasePath ("mma_test_datasize");
    SessionWriter writer;
    writer.open (path, 48000.0, 1, 16, "2026-08-26T14:32:00Z");

    const int numFrames = 100;
    std::vector<float> frames (static_cast<size_t> (numFrames), 0.1f);
    writer.writeInterleaved (frames.data(), static_cast<size_t> (numFrames));
    writer.close();

    std::ifstream f (writer.getCurrentFilePath(), std::ios::binary);
    // fmt chunk (8 + 16) + bext chunk (8 + 602) precede "data" tag.
    std::streampos dataSizePos = 12 + (8 + 16) + (8 + 602) + 4;
    uint32_t dataSize = readU32LE (f, dataSizePos);
    REQUIRE (dataSize == static_cast<uint32_t> (numFrames * 2)); // 1 channel * 2 bytes/sample

    std::remove (writer.getCurrentFilePath().c_str());
}

TEST_CASE (SessionWriter_TracksTotalFramesWritten)
{
    std::string path = tempBasePath ("mma_test_frames");
    SessionWriter writer;
    writer.open (path, 48000.0, 2, 24, "2026-08-26T14:32:00Z");

    std::vector<float> frames (20, 0.0f); // 10 frames of 2 channels
    writer.writeInterleaved (frames.data(), 10);
    REQUIRE (writer.getTotalFramesWritten() == 10);
    writer.close();

    std::remove (writer.getCurrentFilePath().c_str());
}

TEST_CASE (SessionWriter_AutoSplitsAtConfiguredThreshold)
{
    std::string path = tempBasePath ("mma_test_split");
    SessionWriter writer;
    // The detailed rollover behavior is exercised with a tiny private test
    // threshold below. Keep this check for the production-facing initial name:
    // the first file is deliberately unsuffixed and the first continuation is
    // `_001`.
    writer.open (path, 48000.0, 1, 16, "2026-08-26T14:32:00Z");
    REQUIRE (writer.getSplitFileCount() == 0);
    REQUIRE (writer.getCurrentFilePath() == path + ".wav");
    writer.close();

    std::remove (writer.getCurrentFilePath().c_str());
}

TEST_CASE (SessionWriter_SplitFilesCarryTheirPositionFromTheSessionOrigin)
{
    const auto path = tempBasePath ("mma_test_split_reference");
    SessionWriter writer;
    SessionWriterTestAccess::useSplitSize (writer, 4); // mono 16-bit: two frames per file
    REQUIRE (writer.open (path, 48000.0, 1, 16, "2026-09-12T12:00:00Z"));

    const float frames[5] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f };
    REQUIRE (writer.writeInterleaved (frames, 5));
    REQUIRE (writer.close());
    REQUIRE (writer.getSplitFileCount() == 2);

    // RIFF/WAVE (12), fmt chunk (24), bext tag+size (8), then byte 338
    // inside the bext body.
    const std::streampos kTimeReference = 12 + 24 + 8 + 338;

    std::ifstream first (path + ".wav", std::ios::binary);
    std::ifstream second (path + "_001.wav", std::ios::binary);
    std::ifstream third (path + "_002.wav", std::ios::binary);
    REQUIRE (readU64LE (first, kTimeReference) == 0);
    REQUIRE (readU64LE (second, kTimeReference) == 2);
    REQUIRE (readU64LE (third, kTimeReference) == 4);

    first.close();
    second.close();
    third.close();
    std::remove ((path + ".wav").c_str());
    std::remove ((path + "_001.wav").c_str());
    std::remove ((path + "_002.wav").c_str());
}

TEST_CASE (SessionWriter_HeaderRewriteTickUpdatesSizesPeriodically)
{
    std::string path = tempBasePath ("mma_test_tick");
    SessionWriter writer;
    writer.open (path, 48000.0, 1, 16, "2026-08-26T14:32:00Z");

    std::vector<float> frames (1000, 0.2f);
    writer.writeInterleaved (frames.data(), 1000);
    writer.tick (5.5); // exceeds the 5s rewrite interval

    std::ifstream f (writer.getCurrentFilePath(), std::ios::binary);
    std::streampos dataSizePos = 12 + (8 + 16) + (8 + 602) + 4;
    uint32_t dataSize = readU32LE (f, dataSizePos);
    REQUIRE (dataSize == 2000); // 1000 frames * 2 bytes, already flushed by tick()

    writer.close();
    std::remove (writer.getCurrentFilePath().c_str());
}

TEST_CASE (SessionWriter_Writes32BitPcmThatMatchesItsHeader)
{
    // 32-bit was accepted, given a 32-bit header, and packed as 24-bit:
    // every file unreadable. Now the packer writes what the header says.
    const auto path = tempBasePath ("w32");
    SessionWriter writer;
    REQUIRE (writer.open (path, 48000.0, 1, 32, "2026-09-07T00:00:00Z"));

    const float frames[4] = { 0.5f, -0.5f, 1.0f, 0.0f };
    REQUIRE (writer.writeInterleaved (frames, 4));
    writer.close();

    std::ifstream f (path + ".wav", std::ios::binary);
    REQUIRE (f.is_open());
    constexpr std::streamoff kDataSize = 12 + (8 + 16) + (8 + 602) + 4;
    REQUIRE (readU32LE (f, kDataSize) == 4 * 4);

    // Bits per sample and block align in the fmt chunk: offsets 34 and 32.
    f.seekg (34);
    unsigned char bits[2];
    f.read (reinterpret_cast<char*> (bits), 2);
    REQUIRE ((bits[0] | (bits[1] << 8)) == 32);

    // The first sample decodes back to +0.5 full scale.
    const auto first = readU32LE (f, kDataSize + 4);
    const auto value = static_cast<int32_t> (first);
    REQUIRE (value > 1073741000 && value < 1073742000);
}

TEST_CASE (SessionWriter_RefusesADepthItCannotPack)
{
    SessionWriter writer;
    REQUIRE (! writer.open (tempBasePath ("w20"), 48000.0, 1, 20, "2026-09-07T00:00:00Z"));
}

TEST_CASE (SessionWriter_TheFiveSecondHeaderRewriteReportsWhetherItLanded)
{
    // §6.6 rewrites the header every 5 seconds so an interrupted file stays
    // playable. tick() returned void, so the write that keeps a four-hour take
    // recoverable could start failing on a dying card and nothing would notice
    // until the stop -- by which point the header being wrong is the whole loss.
    SessionWriter w;

    // Nothing has been opened, so there is no header to rewrite and the
    // interval falls due immediately.
    REQUIRE_FALSE (w.tick (600.0));

    REQUIRE (w.open (tempBasePath ("tick-report"), 48000.0, 1, 16, "2026-09-08T00:00:00Z"));

    // Inside the interval there is nothing to do, and nothing to report.
    REQUIRE (w.tick (0.1));

    // Past it, the rewrite runs and says it landed.
    REQUIRE (w.tick (600.0));
    REQUIRE (w.close());
}

TEST_CASE (SessionWriter_AHealthyWriterHasNoWriteProblemToReport)
{
    // The reason field exists for the roll-over failure at 3.9 GB, which no
    // test can reach without writing 3.9 GB. What is worth holding is that it
    // stays empty otherwise -- a stale reason shown beside a healthy take would
    // send someone checking a card that is fine.
    SessionWriter w;
    REQUIRE (w.open (tempBasePath ("no-problem"), 48000.0, 1, 16, "2026-09-08T00:00:00Z"));

    std::vector<float> block (128, 0.25f);
    REQUIRE (w.writeInterleaved (block.data(), 128));
    REQUIRE (w.getWriteProblem().empty());

    REQUIRE (w.close());
    REQUIRE (w.getWriteProblem().empty());
}

// The full-drive decision, both ways.
//
// Its two answers have very different consequences. Say "full" when the drive
// is not, and someone whose card was pulled out is told to free up space; stay
// silent when it is, and they are told the drive "stopped responding" and to
// check it is plugged in properly -- so they spend the one moment they are
// still next to the rig re-seating a cable that was never loose. That second
// case is the bug this exists for: an ordinary failed write left no account at
// all, so the card-removal notice spoke for every failure including a full
// card.
//
// Measured in this take's own format rather than as a fixed byte count,
// because 200 kB is a comfortable margin at 16 bits mono and less than a
// tenth of a second at 32 bits across eight channels.
TEST_CASE (SessionWriter_ADriveWithNoRoomForMoreAudioIsFull)
{
    // 24-bit stereo at 48 kHz needs 288,000 bytes for a second.
    REQUIRE (freeSpaceMeansDriveIsFull (0, 3, 2, 48000.0));
    REQUIRE (freeSpaceMeansDriveIsFull (1024, 3, 2, 48000.0));
    REQUIRE (freeSpaceMeansDriveIsFull (287999, 3, 2, 48000.0));

    // The same free space is NOT full for a smaller format: 200 kB is over two
    // seconds at 16 bits mono. A fixed threshold would have got one of these
    // two lines wrong whichever number it picked.
    REQUIRE_FALSE (freeSpaceMeansDriveIsFull (200000, 2, 1, 48000.0));
    REQUIRE (freeSpaceMeansDriveIsFull (200000, 3, 2, 48000.0));
}

TEST_CASE (SessionWriter_ADriveWithRoomLeftIsNotCalledFull)
{
    REQUIRE_FALSE (freeSpaceMeansDriveIsFull (288000, 3, 2, 48000.0));
    REQUIRE_FALSE (freeSpaceMeansDriveIsFull (10ull * 1024 * 1024 * 1024, 3, 2, 48000.0));
    REQUIRE_FALSE (freeSpaceMeansDriveIsFull (1ull << 40, 4, 8, 96000.0));
}

// Nonsense arguments must not make an empty drive look roomy: a zero or
// negative format would collapse bytesPerSecond and report "not full" for a
// drive with nothing on it, which is the silent-loss direction.
TEST_CASE (SessionWriter_AnImpossibleFormatStillTreatsAnEmptyDriveAsFull)
{
    REQUIRE (freeSpaceMeansDriveIsFull (0, 0, 0, 0.0));
    REQUIRE (freeSpaceMeansDriveIsFull (0, -1, -1, -1.0));
}

// A healthy writer with room left says nothing, so the caller's "the card
// stopped accepting writes" account stands -- which is the right answer for a
// drive that has actually been pulled out.
TEST_CASE (SessionWriter_AHealthyWriteLeavesNoFullDriveClaim)
{
    SessionWriter w;
    REQUIRE (w.open (tempBasePath ("room-left"), 48000.0, 1, 16, "2026-09-15T00:00:00Z"));

    std::vector<float> block (128, 0.25f);
    REQUIRE (w.writeInterleaved (block.data(), 128));
    REQUIRE (w.getWriteProblem().empty());

    REQUIRE (w.close());
    REQUIRE (w.getWriteProblem().empty());
}

// RIFF requires every chunk to occupy an even number of bytes: an odd data
// chunk is followed by one pad byte that its size does not count. A mono
// 24-bit stem with an odd frame count used to end on an odd byte, with the
// RIFF size odd too -- strict readers reject that or read the next chunk from
// the wrong offset.
TEST_CASE (SessionWriter_AnOddDataChunkIsPaddedToAnEvenLength)
{
    std::string path = tempBasePath ("mma_test_pad");
    SessionWriter writer;
    writer.open (path, 48000.0, 1, 24, "2026-08-26T14:32:00Z");

    std::vector<float> frames (3, 0.25f); // 3 frames x 3 bytes = 9, odd
    writer.writeInterleaved (frames.data(), frames.size());
    REQUIRE (writer.close());

    std::ifstream f (writer.getCurrentFilePath(), std::ios::binary);
    const std::streampos dataSizePos = 12 + (8 + 16) + (8 + 602) + 4;
    const uint32_t dataSize = readU32LE (f, dataSizePos);
    REQUIRE (dataSize == 9u); // the pad is not counted in the chunk's size

    f.seekg (0, std::ios::end);
    const auto fileSize = static_cast<uint64_t> (f.tellg());
    REQUIRE (fileSize % 2 == 0);
    REQUIRE (fileSize == static_cast<uint64_t> (dataSizePos) + 4 + 9 + 1);
    REQUIRE (readU32LE (f, 4) == static_cast<uint32_t> (fileSize - 8));

    f.seekg (static_cast<std::streamoff> (fileSize - 1));
    char last = 1;
    f.read (&last, 1);
    REQUIRE (last == 0);

    f.close();
    std::remove (writer.getCurrentFilePath().c_str());
}

// And an even chunk gets no pad.
TEST_CASE (SessionWriter_AnEvenDataChunkIsNotPadded)
{
    std::string path = tempBasePath ("mma_test_nopad");
    SessionWriter writer;
    writer.open (path, 48000.0, 1, 24, "2026-08-26T14:32:00Z");

    std::vector<float> frames (4, 0.25f); // 12 bytes
    writer.writeInterleaved (frames.data(), frames.size());
    REQUIRE (writer.close());

    std::ifstream f (writer.getCurrentFilePath(), std::ios::binary);
    const std::streampos dataSizePos = 12 + (8 + 16) + (8 + 602) + 4;
    f.seekg (0, std::ios::end);
    REQUIRE (static_cast<uint64_t> (f.tellg()) == static_cast<uint64_t> (dataSizePos) + 4 + 12);

    f.close();
    std::remove (writer.getCurrentFilePath().c_str());
}

#if ! defined (_WIN32)
// Renaming the take folder mid-take (in Finder, say, to the guest's name) must
// not end the recording. The 5-second header sync reopened the file by its
// path, which no longer existed, so the sync "failed", tick() returned false,
// and the take was stopped as though the card had been pulled.
TEST_CASE (SessionWriter_RenamingTheTakeFolderMidTakeDoesNotFailTheWriter)
{
    namespace fs = std::filesystem;
    const fs::path before = tempBasePath ("mma_test_rename_before");
    const fs::path after = tempBasePath ("mma_test_rename_after");
    std::error_code ec;
    fs::remove_all (before, ec);
    fs::remove_all (after, ec);
    REQUIRE (fs::create_directory (before));

    SessionWriter writer;
    REQUIRE (writer.open ((before / "01_Guest").string(), 48000.0, 1, 16, "2026-10-06T19:30:00Z"));

    std::vector<float> frames (1000, 0.2f);
    REQUIRE (writer.writeInterleaved (frames.data(), frames.size()));
    REQUIRE (writer.tick (5.5));

    fs::rename (before, after, ec);
    REQUIRE (! ec);

    REQUIRE (writer.writeInterleaved (frames.data(), frames.size()));
    REQUIRE (writer.tick (5.5)); // the header sync still lands
    REQUIRE (writer.close());    // and so does the final one

    std::ifstream f ((after / "01_Guest.wav").string(), std::ios::binary);
    REQUIRE (f.is_open());
    const std::streampos dataSizePos = 12 + (8 + 16) + (8 + 602) + 4;
    REQUIRE (readU32LE (f, dataSizePos) == 4000u); // 2000 frames * 2 bytes
    f.close();

    fs::remove_all (after, ec);
}

// The 3.9 GB roll-over built the next file's name from the path the take was
// opened with. After a mid-take folder rename that folder no longer existed,
// the next file could not be created, and the take stopped under "the card may
// be full" -- the one write path that did not follow the folder.
TEST_CASE (SessionWriter_RollOverAfterTheTakeFolderWasRenamedLandsInTheRenamedFolder)
{
    namespace fs = std::filesystem;
    const fs::path before = tempBasePath ("mma_test_split_rename_before");
    const fs::path after = tempBasePath ("mma_test_split_rename_after");
    std::error_code ec;
    fs::remove_all (before, ec);
    fs::remove_all (after, ec);
    REQUIRE (fs::create_directory (before));

    SessionWriter writer;
    SessionWriterTestAccess::useSplitSize (writer, 4); // mono 16-bit: two frames per file
    REQUIRE (writer.open ((before / "01_Guest").string(), 48000.0, 1, 16, "2026-10-11T19:30:00Z"));

    const float frames[5] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f };
    REQUIRE (writer.writeInterleaved (frames, 2)); // fills the first file exactly

    fs::rename (before, after, ec);
    REQUIRE (! ec);

    REQUIRE (writer.writeInterleaved (frames + 2, 3)); // two roll-overs
    REQUIRE (writer.getWriteProblem().empty());
    REQUIRE (writer.getSplitFileCount() == 2);
    REQUIRE (writer.close());

    REQUIRE (! fs::exists (before));
    REQUIRE (fs::exists (after / "01_Guest.wav"));
    REQUIRE (fs::exists (after / "01_Guest_001.wav"));
    REQUIRE (fs::exists (after / "01_Guest_002.wav"));

    // Each part finished with its own size, and the continuations still carry
    // their place on the session timeline.
    const std::streampos dataSizePos = 12 + (8 + 16) + (8 + 602) + 4;
    const std::streampos timeReferencePos = 12 + 24 + 8 + 338;
    {
        std::ifstream first ((after / "01_Guest.wav").string(), std::ios::binary);
        std::ifstream second ((after / "01_Guest_001.wav").string(), std::ios::binary);
        std::ifstream third ((after / "01_Guest_002.wav").string(), std::ios::binary);
        REQUIRE (readU32LE (first, dataSizePos) == 4u);
        REQUIRE (readU32LE (second, dataSizePos) == 4u);
        REQUIRE (readU32LE (third, dataSizePos) == 2u);
        REQUIRE (readU64LE (second, timeReferencePos) == 2);
        REQUIRE (readU64LE (third, timeReferencePos) == 4);
    }

    fs::remove_all (after, ec);
}

// A folder that is simply gone (deleted, or dragged to another drive, which
// copies and deletes) is not a full card, and saying so sent people to the
// wrong thing.
TEST_CASE (SessionWriter_RollOverIntoAFolderThatIsGoneSaysSoRatherThanBlamingTheCard)
{
    namespace fs = std::filesystem;
    const fs::path folder = tempBasePath ("mma_test_split_folder_gone");
    std::error_code ec;
    fs::remove_all (folder, ec);
    REQUIRE (fs::create_directory (folder));

    SessionWriter writer;
    SessionWriterTestAccess::useSplitSize (writer, 4);
    REQUIRE (writer.open ((folder / "01_Guest").string(), 48000.0, 1, 16, "2026-10-11T19:30:00Z"));

    const float frames[3] = { 0.1f, 0.2f, 0.3f };
    REQUIRE (writer.writeInterleaved (frames, 2));

    fs::remove_all (folder, ec);
    REQUIRE (! ec);

    REQUIRE (! writer.writeInterleaved (frames + 2, 1));
    const auto& why = writer.getWriteProblem();
    REQUIRE (why.find ("can no longer be found") != std::string::npos);
    REQUIRE (why.find ("card may be full") == std::string::npos);
    REQUIRE (! fs::exists (folder));
}
#endif
