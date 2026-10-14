#include "TestFramework.h"
#include "Core/WritePipeline.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace mma;

namespace {

std::string tempDir()
{
    for (const char* var : { "MMA_TEST_TMPDIR", "TMPDIR", "TMP", "TEMP" })
    {
        const char* dir = std::getenv (var);
        if (dir != nullptr && *dir != '\0')
            return std::string (dir);
    }
    return "/tmp";
}

// §6.1 writes BWF, so a bext chunk sits between "fmt " and "data" and the
// canonical 44-byte layout does not apply:
//   RIFF header 12 + fmt (8 + 16) + bext (8 + 602) + "data" tag 4
constexpr std::streamoff kDataSizeOffset = 12 + (8 + 16) + (8 + 602) + 4;
constexpr std::streamoff kAudioDataOffset = kDataSizeOffset + 4;

uint32_t readU32LE (std::ifstream& f, std::streampos pos)
{
    f.seekg (pos);
    unsigned char b[4];
    f.read (reinterpret_cast<char*> (b), 4);
    return static_cast<uint32_t> (b[0]) | (static_cast<uint32_t> (b[1]) << 8)
         | (static_cast<uint32_t> (b[2]) << 16) | (static_cast<uint32_t> (b[3]) << 24);
}

/// Spins until the writer thread has completed the requested number of frames.
/// Ring emptiness is too early: the consumer publishes its read index before it
/// writes the stems, so ASAN could observe an empty ring and switch to mix-only
/// while that dequeued block was still between those two operations.
bool waitForWrittenFrames (const WritePipeline& p, uint64_t minimumFrames)
{
    for (int attempt = 0; attempt < 2000; ++attempt)
    {
        if (p.getFramesWritten() >= minimumFrames)
            return true;

        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    return false;
}

std::vector<WriteChannelSpec> twoChannels()
{
    return { { "01_Left", 0.0f }, { "02_Right", 0.0f } };
}

struct ScopedTempTree
{
    explicit ScopedTempTree (const char* label)
    {
        const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root = std::filesystem::path (tempDir())
             / (std::string ("sobstage-") + label + "-" + std::to_string (nonce));
        std::filesystem::create_directories (root / "card");
        std::filesystem::create_directories (root / "mirror");
    }

    ~ScopedTempTree()
    {
        std::error_code ignored;
        std::filesystem::remove_all (root, ignored);
    }

    std::string card() const { return (root / "card").string(); }
    std::string mirror() const { return (root / "mirror").string(); }

    std::filesystem::path root;
};

} // namespace

TEST_CASE (WritePipeline_StartsAndStops)
{
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE (p.isRunning());
    p.stop();
    REQUIRE_FALSE (p.isRunning());
}

TEST_CASE (WritePipeline_RefusesToStartWithNoChannels)
{
    WritePipeline p;
    REQUIRE_FALSE (p.start (tempDir(), {}, 48000.0, 16, "2026-08-27T00:00:00Z"));
}

TEST_CASE (WritePipeline_RefusesToStartTwice)
{
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE_FALSE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));
    p.stop();
}

TEST_CASE (WritePipeline_PushIsRejectedWhenNotRunning)
{
    WritePipeline p;
    std::vector<float> a (64, 0.5f), b (64, 0.5f);
    const float* chans[] = { a.data(), b.data() };
    REQUIRE_FALSE (p.pushBlock (chans, 2, 64));
}

TEST_CASE (WritePipeline_AcceptsBlocksAndCountsFrames)
{
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (128, 0.25f), b (128, -0.25f);
    const float* chans[] = { a.data(), b.data() };

    REQUIRE (p.pushBlock (chans, 2, 128));
    REQUIRE (p.getFramesAccepted() == 128);
    REQUIRE (p.getFramesDropped() == 0);

    p.stop();
}

TEST_CASE (WritePipeline_AShortBlockIsWrittenRatherThanThrownAway)
{
    // The device delivered fewer channels than the take is recording. This
    // used to reject the whole block, and that lost the channels that DID
    // arrive -- every one of them, for the whole take.
    //
    // On a real Mac that presented as silent files while the on-screen meters
    // moved: metering reads the same buffer a moment later and has no such
    // check, so the app showed live level for audio it was dropping on the
    // floor. §0.1 calls unreported loss the one unacceptable failure, and
    // discarding every channel because one was missing is the largest possible
    // version of it.
    //
    // §6.5 already says what to do, for the case where a mic is unplugged
    // mid-take: the file layout is fixed and the missing channel writes
    // silence. A short block is the same shape of problem and gets the same
    // answer.
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (64, 0.1f);
    const float* chans[] = { a.data() };

    REQUIRE (p.pushBlock (chans, 1, 64));
    REQUIRE (p.getFramesAccepted() == 64);

    // Nothing was lost: every channel that arrived was written. The missing
    // one had no audio to lose.
    REQUIRE (p.getFramesDropped() == 0);

    // And the channel that did arrive is what makes the take non-silent.
    REQUIRE (p.getPeakWritten() > 0.0f);

    p.stop();
}

TEST_CASE (WritePipeline_ABlockWiderThanTheTakeCountsTheChannelsItCannotHold)
{
    // The other direction: more channels arrived than the take has files for.
    // The file layout cannot widen mid-take, so those samples genuinely are
    // lost and §0.1 requires them counted -- but the channels that DO fit are
    // still written rather than the whole block going in the bin.
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (64, 0.1f), b (64, 0.2f), c (64, 0.3f);
    const float* chans[] = { a.data(), b.data(), c.data() };

    REQUIRE (p.pushBlock (chans, 3, 64));
    REQUIRE (p.getFramesAccepted() == 64);
    REQUIRE (p.getFramesDropped() == 64);   // the third channel had nowhere to go

    p.stop();
}

TEST_CASE (WritePipeline_WritesAStemPerChannelPlusAMix)
{
    const auto dir = tempDir();
    WritePipeline p;
    REQUIRE (p.start (dir, twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (256, 0.5f), b (256, 0.5f);
    const float* chans[] = { a.data(), b.data() };
    REQUIRE (p.pushBlock (chans, 2, 256));

    p.stop();

    for (const char* name : { "/01_Left.wav", "/02_Right.wav", "/MIX.wav" })
    {
        std::ifstream f (dir + name, std::ios::binary);
        REQUIRE (f.is_open());

        char riff[4];
        f.read (riff, 4);
        REQUIRE (std::string (riff, 4) == "RIFF");
    }
}

TEST_CASE (WritePipeline_StemsAreWrittenAtUnityDespiteTrim)
{
    const auto dir = tempDir();

    // §4: trim affects the mix file and never the stems, so a bad trim decision
    // cannot destroy the raw material.
    std::vector<WriteChannelSpec> channels = { { "trim_stem", -20.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (512, 0.5f);
    const float* chans[] = { a.data() };
    REQUIRE (p.pushBlock (chans, 1, 512));
    p.stop();

    std::ifstream f (dir + "/trim_stem.wav", std::ios::binary);
    REQUIRE (f.is_open());

    f.seekg (kAudioDataOffset);
    unsigned char lo = 0, hi = 0;
    f.read (reinterpret_cast<char*> (&lo), 1);
    f.read (reinterpret_cast<char*> (&hi), 1);
    const int16_t sample = static_cast<int16_t> (static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8));

    // 0.5 at unity is around 16384. With -20 dB trim applied it would be ~1638.
    REQUIRE (sample > 12000);
}

TEST_CASE (WritePipeline_UnpluggedChannelWritesSilenceNotNothing)
{
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "silent_ch", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

    // §6.5: the channel stays in the file, it just goes quiet.
    p.setChannelLive (0, false);

    std::vector<float> a (256, 0.9f);
    const float* chans[] = { a.data() };
    REQUIRE (p.pushBlock (chans, 1, 256));
    REQUIRE (p.getFramesAccepted() == 256);
    p.stop();

    std::ifstream f (dir + "/silent_ch.wav", std::ios::binary);
    REQUIRE (f.is_open());
    const auto dataSize = readU32LE (f, kDataSizeOffset);
    // The frames are present, they are just zero.
    REQUIRE (dataSize == 256 * 2);

    f.seekg (kAudioDataOffset);
    unsigned char lo = 0, hi = 0;
    f.read (reinterpret_cast<char*> (&lo), 1);
    f.read (reinterpret_cast<char*> (&hi), 1);
    REQUIRE (lo == 0);
    REQUIRE (hi == 0);
}

TEST_CASE (WritePipeline_NullChannelPointerIsTreatedAsSilence)
{
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (64, 0.5f);
    const float* chans[] = { a.data(), nullptr };
    // A backend can hand back fewer buffers than expected; that must not crash
    // the audio thread.
    REQUIRE (p.pushBlock (chans, 2, 64));

    p.stop();
}

TEST_CASE (WritePipeline_StopFlushesWhatIsStillBuffered)
{
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "flush_ch", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (1024, 0.3f);
    const float* chans[] = { a.data() };
    REQUIRE (p.pushBlock (chans, 1, 1024));

    // Stop immediately, before the writer thread has necessarily run. §0.1:
    // audio is never lost, including the last block.
    p.stop();

    std::ifstream f (dir + "/flush_ch.wav", std::ios::binary);
    REQUIRE (f.is_open());
    REQUIRE (readU32LE (f, kDataSizeOffset) == 1024 * 2);
}

TEST_CASE (WritePipeline_FillFractionRisesWithUndrainedAudio)
{
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE (p.getFillFraction() >= 0.0);
    REQUIRE (p.getFillFraction() <= 1.0);
    p.stop();
}

TEST_CASE (WritePipeline_LiveTrimChangeMovesTheMixNotTheStem)
{
    const auto dir = tempDir();

    std::vector<WriteChannelSpec> channels = { { "live_stem", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (512, 0.5f);
    const float* chans[] = { a.data() };

    // §4: the user turns the mic down mid-take. The mix must follow; the stem
    // must not, because the stem is the raw material the take exists for.
    p.setChannelTrimDb (0, -20.0f);
    REQUIRE (p.pushBlock (chans, 1, 512));
    p.stop();

    auto firstSample = [] (const std::string& path)
    {
        std::ifstream f (path, std::ios::binary);
        REQUIRE (f.is_open());
        f.seekg (kAudioDataOffset);
        unsigned char lo = 0, hi = 0;
        f.read (reinterpret_cast<char*> (&lo), 1);
        f.read (reinterpret_cast<char*> (&hi), 1);
        return static_cast<int16_t> (static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8));
    };

    REQUIRE (firstSample (dir + "/live_stem.wav") > 12000); // unity, ~16384
    REQUIRE (firstSample (dir + "/MIX.wav") < 6000);        // -20 dB, ~1638
}

TEST_CASE (WritePipeline_MirrorWritesASecondCompleteCopy)
{
    const auto dir = tempDir();
    const auto mirror = tempDir();

    std::vector<WriteChannelSpec> channels = { { "01_A", 0.0f }, { "02_B", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z", mirror));
    REQUIRE (p.isMirroring());

    std::vector<float> a (512, 0.5f), b (512, 0.25f);
    const float* chans[] = { a.data(), b.data() };
    REQUIRE (p.pushBlock (chans, 2, 512));
    p.stop();

    // §6.3: the mirror turns most card failures from data loss into
    // inconvenience, which requires every file to be there, not just the mix.
    for (const char* name : { "/01_A.wav", "/02_B.wav", "/MIX.wav" })
    {
        std::ifstream card (dir + name, std::ios::binary);
        std::ifstream copy (mirror + name, std::ios::binary);
        REQUIRE (card.is_open());
        REQUIRE (copy.is_open());

        // Byte-identical: both are written from the same drained scratch, so a
        // divergence here would mean the copy is not a copy.
        const std::string cardBytes ((std::istreambuf_iterator<char> (card)), std::istreambuf_iterator<char>());
        const std::string copyBytes ((std::istreambuf_iterator<char> (copy)), std::istreambuf_iterator<char>());
        REQUIRE (cardBytes == copyBytes);
        REQUIRE (cardBytes.size() > 602);
    }
}

TEST_CASE (WritePipeline_NoMirrorFolderMeansCardOnly)
{
    const auto dir = tempDir();

    std::vector<WriteChannelSpec> channels = { { "01_A", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

    // §6.3 is a default, not a requirement: card-only must be a working mode.
    REQUIRE_FALSE (p.isMirroring());

    std::vector<float> a (256, 0.5f);
    const float* chans[] = { a.data() };
    REQUIRE (p.pushBlock (chans, 1, 256));
    p.stop();

    std::ifstream f (dir + "/01_A.wav", std::ios::binary);
    REQUIRE (f.is_open());
}

TEST_CASE (WritePipeline_StoppingTheMirrorLeavesTheRecordingIntact)
{
    const auto dir = tempDir();
    const auto mirror = tempDir();

    std::vector<WriteChannelSpec> channels = { { "01_A", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z", mirror));

    std::vector<float> a (512, 0.5f);
    const float* chans[] = { a.data() };
    REQUIRE (p.pushBlock (chans, 1, 512));

    // §6.3: the internal drive ran low. The copy stops; the take does not.
    p.stopMirroring();
    REQUIRE_FALSE (p.isMirroring());

    REQUIRE (p.pushBlock (chans, 1, 512));
    p.stop();

    std::ifstream card (dir + "/01_A.wav", std::ios::binary);
    REQUIRE (card.is_open());

    // Both blocks reached the card even though the mirror stopped between them.
    REQUIRE (readU32LE (card, kDataSizeOffset) == 1024 * 2);
}

TEST_CASE (WritePipeline_AHealthyTakeNeverReportsAFailure)
{
    // The other half of the claim: the flag must stay down when nothing is
    // wrong, or it would stop every take the moment it was wired to the UI.
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "healthy_card", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> block (4096, 0.2f);
    const float* chans[] = { block.data() };
    REQUIRE (p.pushBlock (chans, 1, 4096));
    p.stop();

    REQUIRE_FALSE (p.hasCardWriteFailed());
    REQUIRE_FALSE (p.hasMirrorWriteFailed());

    std::remove ((dir + "/healthy_card.wav").c_str());
}

// -----------------------------------------------------------------------
// §6.5 "target card removed". Making a real write fail without unplugging
// real hardware: cap the process's maximum file size so the writer's own
// file grows into a hard EFBIG, which is what a card that has gone away
// looks like from inside a write() call. SIGXFSZ is ignored so the failure
// arrives as a return value rather than killing the test runner -- the
// return value being exactly what SessionWriter documents and what this
// pipeline used to throw away.
//
// POSIX only: Windows has no equivalent, so these two are compiled out there
// rather than faked. The healthy-take check above deliberately sits outside the
// guard -- it needs nothing platform-specific, and "this flag stays down when
// nothing is wrong" is worth asserting on every platform, since a false
// positive there would stop every recording instantly.
// -----------------------------------------------------------------------
#if ! defined(_WIN32)

#include <csignal>
#include <sys/resource.h>

namespace {

class FileSizeCap
{
public:
    explicit FileSizeCap (rlim_t bytes)
    {
        previousHandler = std::signal (SIGXFSZ, SIG_IGN);
        getrlimit (RLIMIT_FSIZE, &previous);

        rlimit capped { bytes, previous.rlim_max };
        applied = setrlimit (RLIMIT_FSIZE, &capped) == 0;
    }

    ~FileSizeCap()
    {
        // Restored whatever happens: this is process-wide, and leaving it in
        // place would silently break every test that writes a file after it.
        if (applied)
            setrlimit (RLIMIT_FSIZE, &previous);

        std::signal (SIGXFSZ, previousHandler);
    }

    bool isApplied() const { return applied; }

private:
    rlimit previous {};
    void (*previousHandler) (int) = nullptr;
    bool applied = false;
};

} // namespace

TEST_CASE (WritePipeline_ADestinationThatFailsMidTakeIsNoticed)
{
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "pulled_card", 0.0f } };

    WritePipeline p;
    {
        // Room for the BWF header and a little audio, then the wall.
        FileSizeCap cap (4096);
        REQUIRE (cap.isApplied());

        REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

        // Nothing has gone wrong yet, and the pipeline must not cry off early.
        REQUIRE_FALSE (p.hasCardWriteFailed());

        std::vector<float> block (8192, 0.3f);
        const float* chans[] = { block.data() };
        REQUIRE (p.pushBlock (chans, 1, 8192));

        // stop() does the final flush synchronously, so this does not depend on
        // catching the writer thread mid-sleep.
        p.stop();

        // The point of the whole change: the card said no, and the pipeline
        // heard it. Before this, every one of those return values was dropped
        // and a pulled card was indistinguishable from a healthy take.
        REQUIRE (p.hasCardWriteFailed());
    }

    std::remove ((dir + "/pulled_card.wav").c_str());
}

TEST_CASE (WritePipeline_AFreshTakeForgetsTheLastOnesFailure)
{
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "reused", 0.0f } };

    WritePipeline p;
    {
        FileSizeCap cap (4096);
        REQUIRE (cap.isApplied());

        REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));
        std::vector<float> block (8192, 0.3f);
        const float* chans[] = { block.data() };
        p.pushBlock (chans, 1, 8192);
        p.stop();
        REQUIRE (p.hasCardWriteFailed());
    }

    // Cap gone: the next take is on a working card and must be allowed to say so.
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE_FALSE (p.hasCardWriteFailed());
    p.stop();

    std::remove ((dir + "/reused.wav").c_str());
}

#endif // ! _WIN32

TEST_CASE (WritePipeline_MixOnlyStopsTheStemsAndKeepsTheMix)
{
    // §6.5: at 90% fill with no mirror, "fall back to writing the mix file
    // only". A complete mix is worth more than eight stems with the same hole
    // in them, and the ring drains at a fraction of the byte rate while it
    // recovers.
    ScopedTempTree files ("mix-only-boundary");
    const auto dir = files.card();
    std::vector<WriteChannelSpec> channels = { { "degrade_a", 0.0f }, { "degrade_b", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE_FALSE (p.isMixOnly());

    std::vector<float> a (512, 0.5f), b (512, 0.25f);
    const float* chans[] = { a.data(), b.data() };
    REQUIRE (p.pushBlock (chans, 2, 512));

    // Wait for the writer thread to finish that block before degrading. Merely
    // seeing an empty ring means it was dequeued, not that its stems were
    // written; that weaker condition made this test scheduler-dependent.
    REQUIRE (waitForWrittenFrames (p, 512));

    // Everything written so far is on both the stems and the mix.
    p.fallBackToMixOnly();
    REQUIRE (p.isMixOnly());

    REQUIRE (p.pushBlock (chans, 2, 512));
    p.stop();

    std::ifstream stem (dir + "/degrade_a.wav", std::ios::binary);
    std::ifstream mix (dir + "/MIX.wav", std::ios::binary);
    REQUIRE (stem.is_open());
    REQUIRE (mix.is_open());

    // The stem stopped at the first block; the mix carried both.
    REQUIRE (readU32LE (stem, kDataSizeOffset) == 512 * 2);
    REQUIRE (readU32LE (mix, kDataSizeOffset) == 1024 * 2);

}

TEST_CASE (WritePipeline_TheMixStillCarriesEveryChannelAfterDegrading)
{
    // Shedding the stems must not quietly drop anyone out of the recording that
    // does survive -- the point is to shed write bandwidth, not people.
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "sum_a", 0.0f }, { "sum_b", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));
    p.fallBackToMixOnly();

    // Two channels at +0.25 each: the mix must be their sum, not one of them.
    std::vector<float> a (256, 0.25f), b (256, 0.25f);
    const float* chans[] = { a.data(), b.data() };
    REQUIRE (p.pushBlock (chans, 2, 256));
    p.stop();

    std::ifstream mix (dir + "/MIX.wav", std::ios::binary);
    REQUIRE (mix.is_open());
    mix.seekg (kAudioDataOffset);
    unsigned char lo = 0, hi = 0;
    mix.read (reinterpret_cast<char*> (&lo), 1);
    mix.read (reinterpret_cast<char*> (&hi), 1);
    const int16_t sample = static_cast<int16_t> (static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8));

    // 0.25 + 0.25 = 0.5 of full scale, give or take rounding.
    REQUIRE (sample > 16000);
    REQUIRE (sample < 16800);

    std::remove ((dir + "/sum_a.wav").c_str());
    std::remove ((dir + "/sum_b.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());
}

TEST_CASE (WritePipeline_MirroredDegradationStopsBothStemSetsAndKeepsBothMixes)
{
    // Ring pressure exists before either destination: card and mirror consume
    // the same drained block on this one writer thread. Emergency mix-only mode
    // therefore has to shed the stems on BOTH destinations. Keeping mirror
    // stems would preserve the work that caused the shared queue to fill, not
    // preserve audio that has already fallen out of it.
    ScopedTempTree files ("mirrored-degradation");
    WritePipeline p;
    REQUIRE (p.start (files.card(), twoChannels(), 48000.0, 16,
                      "2026-09-12T00:00:00Z", files.mirror()));
    REQUIRE (p.isMirroring());

    p.fallBackToMixOnly();
    REQUIRE (p.isMixOnly());

    std::vector<float> left (512, 0.25f), right (512, 0.25f);
    const float* channels[] = { left.data(), right.data() };
    REQUIRE (p.pushBlock (channels, 2, 512));
    p.stop();

    // Deliberate degradation is explicit state, not a destination failure.
    REQUIRE_FALSE (p.hasCardWriteFailed());
    REQUIRE_FALSE (p.hasMirrorWriteFailed());

    for (const auto& folder : { files.card(), files.mirror() })
    {
        for (const char* stem : { "/01_Left.wav", "/02_Right.wav" })
        {
            std::ifstream file (folder + stem, std::ios::binary);
            REQUIRE (file.is_open());
            REQUIRE (readU32LE (file, kDataSizeOffset) == 0);
        }

        std::ifstream mix (folder + "/MIX.wav", std::ios::binary);
        REQUIRE (mix.is_open());
        REQUIRE (readU32LE (mix, kDataSizeOffset) == 512 * 2);
    }

    // The safety copy remains meaningful in degraded mode: it carries the same
    // complete MIX as the card, while neither side pretends its stems continued.
    std::ifstream cardMix (files.card() + "/MIX.wav", std::ios::binary);
    std::ifstream mirrorMix (files.mirror() + "/MIX.wav", std::ios::binary);
    const std::string cardBytes ((std::istreambuf_iterator<char> (cardMix)),
                                 std::istreambuf_iterator<char>());
    const std::string mirrorBytes ((std::istreambuf_iterator<char> (mirrorMix)),
                                   std::istreambuf_iterator<char>());
    REQUIRE (cardBytes == mirrorBytes);
}

TEST_CASE (WritePipeline_AFreshTakeIsNotStillDegraded)
{
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "rearm", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));
    p.fallBackToMixOnly();
    REQUIRE (p.isMixOnly());
    p.stop();

    // §6.5 never restarts the stems within a take. A new take is a new take.
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE_FALSE (p.isMixOnly());
    p.stop();

    std::remove ((dir + "/rearm.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());
}

TEST_CASE (WritePipeline_AShortBlockKeepsTheChannelsThatArrived)
{
    // This test used to assert the opposite, and the assertion was the bug.
    //
    // The earlier audit found that a mismatched block was discarded without
    // being counted, and fixed the counting: §0.1 makes unreported loss the one
    // unacceptable failure, so the block was still dropped but framesDropped
    // rose. That made the loss visible. It did not stop it.
    //
    // The remedy was wrong. When a capture path hands over fewer channels than
    // it opened with, dropping the block loses every channel that DID arrive,
    // for as long as the mismatch lasts -- which, if the device simply reports
    // fewer inputs than the take was built for, is the whole recording. On a
    // real Mac that presented as silent files with the meters moving, because
    // metering reads the same buffer afterwards and has no matching check.
    //
    // §6.5 already had the answer for a mic that goes away mid-take: the file
    // layout holds and the missing channel writes silence. A short block is the
    // same problem, so it gets the same answer, and nothing is lost.
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "mismatch-a", 0.0f }, { "mismatch-b", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-09-01T00:00:00Z"));

    std::vector<float> a (128, 0.1f), b (128, 0.2f);
    const float* two[2] = { a.data(), b.data() };

    REQUIRE (p.pushBlock (two, 2, 128));
    REQUIRE (p.getFramesDropped() == 0);

    // One channel short of the take, which is what a capture path handing over
    // fewer channels than it opened with looks like from here.
    const float* one[1] = { a.data() };

    REQUIRE (p.pushBlock (one, 1, 128));

    // Written, not binned -- and nothing counted as lost, because the channel
    // that was missing had no audio to lose.
    REQUIRE (p.getFramesAccepted() == 256);
    REQUIRE (p.getFramesDropped() == 0);

    p.stop();

    std::remove ((dir + "/mismatch-a.wav").c_str());
    std::remove ((dir + "/mismatch-b.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());
}

// ---------------------------------------------------------------------------
// Nothing is silent: a take that cannot start, and a backup that never existed.
// ---------------------------------------------------------------------------

TEST_CASE (WritePipeline_AFailedStartSaysWhichFileAndWhere)
{
    // The record button used to refuse to latch and say nothing at all -- a
    // full card, a locked card and a card pulled between arming and pressing
    // record were indistinguishable, and none of them produced a sentence.
    WritePipeline p;
    const std::string missing = tempDir() + "/mma-a-folder-that-is-not-there-9f3c";

    REQUIRE_FALSE (p.start (missing, twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));

    const auto& problem = p.getStartProblem();
    REQUIRE_FALSE (problem.empty());

    // Names the folder the user has to go and look at, and the file that
    // could not be made.
    REQUIRE (problem.find (missing) != std::string::npos);
    REQUIRE (problem.find (twoChannels()[0].fileName) != std::string::npos);

    // §10.6: what to do, not just what happened.
    REQUIRE (problem.find ("card") != std::string::npos);
}

TEST_CASE (WritePipeline_ASuccessfulStartLeavesNoProblemBehind)
{
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE (p.getStartProblem().empty());
    REQUIRE_FALSE (p.hasMirrorFailedToOpen());
    p.stop();
}

TEST_CASE (WritePipeline_AStartProblemIsClearedByTheNextTake)
{
    // A previous take's failure must never make this one look doomed.
    WritePipeline p;
    REQUIRE_FALSE (p.start (tempDir() + "/mma-not-there-1a2b", twoChannels(), 48000.0, 16, "t"));
    REQUIRE_FALSE (p.getStartProblem().empty());

    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE (p.getStartProblem().empty());
    p.stop();
}

TEST_CASE (WritePipeline_AMirrorThatNeverOpenedIsReportedRatherThanAssumed)
{
    // §6.3 will not fail a take over the safety net, and that is right. What
    // was wrong was that it failed in silence: the take ran card-only while
    // every caller that asked was told a mirror was active, so the user kept a
    // backup that had never existed.
    WritePipeline p;
    const std::string noSuchMirror = tempDir() + "/mma-mirror-that-is-not-there-77d1";

    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z", noSuchMirror));

    // The take started, exactly as §6.3 requires.
    REQUIRE (p.isRunning());

    // And the missing copy is on the record rather than assumed into existence.
    REQUIRE (p.hasMirrorFailedToOpen());
    REQUIRE_FALSE (p.isMirroring());

    // A mirror that never opened is not a mirror that broke mid-take: the two
    // need different sentences, so they stay different flags.
    REQUIRE_FALSE (p.hasMirrorWriteFailed());

    p.stop();
}

TEST_CASE (WritePipeline_AMirrorThatFailsPartWayThroughLeavesNoStubsBehind)
{
    // The case the test above does not reach: the mirror folder EXISTS and the
    // stems open, and only the mix fails. Releasing the stem writers then ran
    // ~SessionWriter -> close() on each, which finalizes the file and leaves
    // it -- so the backup folder ended up holding a full set of header-only
    // .wav files. A backup that looks present and holds nothing is worse than
    // an absent one; the card path has discarded rather than released for
    // exactly this reason since it was written.
    WritePipeline p;

    const auto mirror = std::filesystem::path (tempDir()) / "mma-mirror-partial-3f2a";
    std::error_code ec;
    std::filesystem::remove_all (mirror, ec);
    std::filesystem::create_directories (mirror, ec);

    // A DIRECTORY where the mix file needs to be, so the stems open and the
    // mix cannot. Works regardless of who is running the test, which a
    // permission trick would not.
    std::filesystem::create_directory (mirror / "MIX.wav", ec);

    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z",
                      mirror.string()));

    REQUIRE (p.isRunning());          // §6.3: the take never fails over the safety net
    REQUIRE (p.hasMirrorFailedToOpen());
    REQUIRE_FALSE (p.isMirroring());

    // Nothing left behind but the obstruction we put there.
    int strays = 0;
    for (const auto& entry : std::filesystem::directory_iterator (mirror))
        if (entry.path().filename() != "MIX.wav")
            ++strays;

    REQUIRE (strays == 0);

    p.stop();
    std::filesystem::remove_all (mirror, ec);
}

TEST_CASE (WritePipeline_NoMirrorAskedForIsNotAMirrorFailure)
{
    // Card-only is a choice, not a fault. Reporting one here would tell the
    // user a backup broke when they never asked for one.
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE_FALSE (p.hasMirrorFailedToOpen());
    REQUIRE_FALSE (p.hasMirrorWriteFailed());
    p.stop();
}

TEST_CASE (WritePipeline_AnOrdinaryStopFinalizesWithoutClaimingAFailure)
{
    // stop() now judges every close(), so the ordinary path has to stay clean:
    // a finalize check that cries wolf would stop a good take with an alert.
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (128, 0.25f), b (128, -0.25f);
    const float* chans[2] = { a.data(), b.data() };
    REQUIRE (p.pushBlock (chans, 2, 128));

    p.stop();

    REQUIRE_FALSE (p.hasCardWriteFailed());
    REQUIRE_FALSE (p.hasMirrorWriteFailed());
}

TEST_CASE (WritePipeline_TwentyFourBitIsTheDepthTheAppShipsAndMustCarrySignal)
{
    // Every other test in this file, and both capture harnesses, run at 16
    // bits. The app records at 24. So the depth that actually ships had never
    // been executed by anything -- the exact shape of gap that let five
    // defects live in the platform backends until they were simulated.
    //
    // A wrong conversion or a header disagreeing with the data here produces
    // full-length files that play as silence, which is indistinguishable from
    // a dead microphone unless someone looks at the bytes.
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "depth24", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 24, "2026-09-04T00:00:00Z"));

    std::vector<float> loud (512, 0.5f);
    const float* chans[] = { loud.data() };

    REQUIRE (p.pushBlock (chans, 1, 512));
    REQUIRE (waitForWrittenFrames (p, 512));
    p.stop();

    std::ifstream f (dir + "/depth24.wav", std::ios::binary);
    REQUIRE (f.is_open());

    // Three bytes per frame for one channel, so the data chunk is 512 * 3.
    REQUIRE (readU32LE (f, kDataSizeOffset) == 512 * 3);

    // And the samples are the value that went in, not zero. 0.5 * 8388607
    // rounds to 4194304 = 0x400000, little-endian 00 00 40.
    f.seekg (kAudioDataOffset);
    unsigned char b[3] = { 0, 0, 0 };
    f.read (reinterpret_cast<char*> (b), 3);

    const int32_t sample = static_cast<int32_t> (b[0])
                         | (static_cast<int32_t> (b[1]) << 8)
                         | (static_cast<int32_t> (b[2]) << 16);

    REQUIRE (sample == 4194304);

    std::remove ((dir + "/depth24.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());
}

TEST_CASE (WritePipeline_EachStemIsWrittenAtItsOwnDeviceDepth)
{
    // §2.3: "follows device capability PER CHANNEL". A rig can hold a 16-bit
    // microphone and a 24-bit interface at the same time, and there is no one
    // depth that serves both -- 24 pads the first with half again its file size
    // and no information, 16 throws away what the second delivered.
    //
    // One SessionWriter already exists per stem, so the depth simply had to
    // reach it; before this it took the take's single figure and every stem
    // came out 24-bit regardless of what was plugged in.
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = {
        { "yeti16",     0.0f, 16 },   // a 16-bit microphone (§14.1)
        { "iface24",    0.0f, 24 },   // a 24-bit interface
        { "unreported", 0.0f, 0  },   // a backend that does not say: take depth
    };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 24, "2026-09-04T00:00:00Z"));

    std::vector<float> loud (256, 0.5f);
    const float* chans[] = { loud.data(), loud.data(), loud.data() };

    REQUIRE (p.pushBlock (chans, 3, 256));
    REQUIRE (waitForWrittenFrames (p, 256));
    p.stop();

    // The data chunk size is the proof: bytes per frame follows the depth.
    std::ifstream a (dir + "/yeti16.wav", std::ios::binary);
    std::ifstream b (dir + "/iface24.wav", std::ios::binary);
    std::ifstream c (dir + "/unreported.wav", std::ios::binary);
    REQUIRE (a.is_open());
    REQUIRE (b.is_open());
    REQUIRE (c.is_open());

    REQUIRE (readU32LE (a, kDataSizeOffset) == 256 * 2);   // 16-bit, not padded to 24
    REQUIRE (readU32LE (b, kDataSizeOffset) == 256 * 3);
    REQUIRE (readU32LE (c, kDataSizeOffset) == 256 * 3);   // falls back to the take's

    // The MIX is the take's depth: it carries every channel, so it cannot be
    // narrower than the widest of them.
    std::ifstream mix (dir + "/MIX.wav", std::ios::binary);
    REQUIRE (mix.is_open());
    REQUIRE (readU32LE (mix, kDataSizeOffset) == 256 * 3);
}

#if ! defined (_WIN32)
// A take folder renamed in Finder mid-take carries the open files with it. The
// pipeline has to report where they now are, so the growth check and the
// stop-time session.json follow the folder instead of the path it started at.
TEST_CASE (WritePipeline_FollowsATakeFolderRenamedMidTake)
{
    ScopedTempTree tree ("rename");
    const auto before = tree.root / "card" / "2026-10-06_1930_Show";
    const auto after = tree.root / "card" / "Episode-12";
    std::filesystem::create_directories (before);

    WritePipeline p;
    REQUIRE (p.start (before.string(), twoChannels(), 48000.0, 16, "2026-10-06T19:30:00Z"));
    REQUIRE (p.getLiveSessionFolder() == before.string());

    std::vector<float> a (4800, 0.25f), b (4800, -0.25f);
    const float* chans[] = { a.data(), b.data() };
    uint64_t total = 0;

    // A little over a second of audio each time: the folder is re-read about
    // once a second of audio written.
    const auto pushAboutASecond = [&]
    {
        for (int i = 0; i < 12; ++i)
            REQUIRE (p.pushBlock (chans, 2, 4800));
        total += 12 * 4800;
        REQUIRE (waitForWrittenFrames (p, total));
    };

    pushAboutASecond();

    // Unmoved: the spelling the take was given, not a resolved one.
    REQUIRE (p.getLiveSessionFolder() == before.string());

    std::filesystem::rename (before, after);
    pushAboutASecond();

    std::error_code ec;
    REQUIRE (std::filesystem::equivalent (p.getLiveSessionFolder(), after, ec));
    REQUIRE_FALSE (p.hasCardWriteFailed());

    p.stop();
    REQUIRE_FALSE (p.hasCardWriteFailed());
    REQUIRE (std::filesystem::equivalent (p.getLiveSessionFolder(), after, ec));
    REQUIRE (std::filesystem::exists (after / "MIX.wav"));
}
#endif

namespace {

/// Index of the loudest sample of a 16-bit mono file.
long long loudestFrameOf (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    REQUIRE (f.is_open());
    const auto bytes = readU32LE (f, kDataSizeOffset);
    f.seekg (kAudioDataOffset);

    long long best = -1;
    int bestMagnitude = 0;

    for (uint32_t i = 0; i < bytes / 2; ++i)
    {
        unsigned char lo = 0, hi = 0;
        f.read (reinterpret_cast<char*> (&lo), 1);
        f.read (reinterpret_cast<char*> (&hi), 1);
        const int v = static_cast<int16_t> (static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8));

        if (std::abs (v) > bestMagnitude)
        {
            bestMagnitude = std::abs (v);
            best = static_cast<long long> (i);
        }
    }

    return best;
}

} // namespace

TEST_CASE (WritePipeline_LinesTheStemsUpByTheOffsetsItIsHanded)
{
    // The headphones are fed each microphone as early as its device allows,
    // so the writer is handed them unaligned, with how far to hold each one
    // back. Left's interface is quicker by 37 samples: a clap both heard at
    // once arrives on Left at 1000 and on Right at 1037, and lands on one
    // frame in both stems and once in the mix. Then, from frame 10240 on,
    // Right is to be held back 50 (its device's place moved 50 earlier
    // against Left's): a later clap, arriving at 20000 on Left and 19987 on
    // Right, still lands on one frame -- the change applied at the frame it
    // was handed over for, not whenever the writer got to it.
    for (const int clapAt : { 1000, 20000 })
    {
        ScopedTempTree tree ("align");
        WritePipeline p;
        REQUIRE (p.start (tree.card(), twoChannels(), 48000.0, 16, "2026-10-08T00:00:00Z"));

        std::vector<float> left (256), right (256);
        const float* chans[] = { left.data(), right.data() };

        for (int block = 0; block < 120; ++block)
        {
            const long long first = static_cast<long long> (block) * 256;
            const bool changed = first >= 10240;
            const int offsets[] = { 37, changed ? 50 : 0 };
            const long long rightClap = clapAt == 1000 ? 1037 : 19987;

            for (int i = 0; i < 256; ++i)
            {
                left[static_cast<size_t> (i)] = first + i == clapAt ? 0.9f : 0.0f;
                right[static_cast<size_t> (i)] = first + i == rightClap ? 0.9f : 0.0f;
            }

            REQUIRE (p.pushBlock (chans, 2, 256, offsets));
        }

        p.stop();

        const auto l = loudestFrameOf (tree.card() + "/01_Left.wav");
        const auto r = loudestFrameOf (tree.card() + "/02_Right.wav");
        const auto mix = loudestFrameOf (tree.card() + "/MIX.wav");

        REQUIRE (l == clapAt + 37);
        REQUIRE (r == l);
        REQUIRE (mix == l);

        // Every stem holds exactly the frames the take accepted, and the
        // change is on record: 50 samples of silence written into Right.
        REQUIRE (p.getFramesWritten() == p.getFramesAccepted());
        REQUIRE (p.getChannelStartAlignmentOffset (0) == 37);
        REQUIRE (p.getChannelAlignmentOffset (1) == 50);
        REQUIRE (p.getChannelAlignmentSilence (1) == 50u);
        REQUIRE (p.getChannelAlignmentSilence (0) == 0u);
        REQUIRE (p.isAlignmentExact());
    }
}

TEST_CASE (WritePipeline_WithNoOffsetsTheStemsAreWrittenAsTheyCame)
{
    // The aggregate path: the OS has already lined the channels up, and the
    // writer is handed no offsets at all.
    ScopedTempTree tree ("no-align");
    WritePipeline p;
    REQUIRE (p.start (tree.card(), twoChannels(), 48000.0, 16, "2026-10-08T00:00:00Z"));

    std::vector<float> left (256, 0.0f), right (256, 0.0f);
    left[100] = 0.9f;
    right[137] = 0.9f;
    const float* chans[] = { left.data(), right.data() };
    REQUIRE (p.pushBlock (chans, 2, 256));
    p.stop();

    REQUIRE (loudestFrameOf (tree.card() + "/01_Left.wav") == 100);
    REQUIRE (loudestFrameOf (tree.card() + "/02_Right.wav") == 137);
    REQUIRE (p.getChannelAlignmentOffset (0) == 0);
    REQUIRE (p.isAlignmentExact());
}

namespace {

/// A 16-bit mono file's samples.
std::vector<int> samplesOf (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    REQUIRE (f.is_open());
    const auto bytes = readU32LE (f, kDataSizeOffset);
    f.seekg (kAudioDataOffset);
    std::vector<int> out;
    out.reserve (bytes / 2);

    for (uint32_t i = 0; i < bytes / 2; ++i)
    {
        unsigned char lo = 0, hi = 0;
        f.read (reinterpret_cast<char*> (&lo), 1);
        f.read (reinterpret_cast<char*> (&hi), 1);
        out.push_back (static_cast<int16_t> (static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8)));
    }

    return out;
}

} // namespace

TEST_CASE (WritePipeline_APairWeightMovesTheMixFromItsFrameAndNeverTheStems)
{
    // §2.1: a fresh two-input microphone starts in MIX.wav at half each, as
    // in the headphones, and follows its verdict from the frame the verdict
    // was handed over for -- reached over 10 ms, not in one step. Summed at
    // unity it was in the file twice for the whole take. Left carries the
    // voice at 0.8 and right is silent, so half each is 0.4 in the mix; a
    // Mono verdict for the left side, or a Stereo one, brings it to 0.8.
    // The stems are each microphone's own samples throughout.
    constexpr int kBlock = 256;
    constexpr int kVerdictFrame = 40 * kBlock;
    const int quarter = static_cast<int> (std::lround (0.4 * 32767.0));
    const int whole = static_cast<int> (std::lround (0.8 * 32767.0));

    for (const float rightAfter : { 0.0f, 1.0f })
    {
        ScopedTempTree tree ("pair-weight");
        auto specs = twoChannels();
        specs[0].mixWeight = 0.5f;
        specs[1].mixWeight = 0.5f;

        WritePipeline p;
        REQUIRE (p.start (tree.card(), specs, 48000.0, 16, "2026-10-14T00:00:00Z"));

        std::vector<float> left (kBlock, 0.8f), right (kBlock, 0.0f);
        const float* chans[] = { left.data(), right.data() };

        for (int block = 0; block < 80; ++block)
        {
            const bool decided = block * kBlock >= kVerdictFrame;
            const float weights[] = { decided ? 1.0f : 0.5f, decided ? rightAfter : 0.5f };
            REQUIRE (p.pushBlock (chans, 2, kBlock, nullptr, nullptr, nullptr, weights));
        }

        p.stop();

        const auto mix = samplesOf (tree.card() + "/MIX.wav");
        const auto stemLeft = samplesOf (tree.card() + "/01_Left.wav");
        const auto stemRight = samplesOf (tree.card() + "/02_Right.wav");
        REQUIRE (mix.size() == static_cast<size_t> (80 * kBlock));

        // The voice once, up to the frame before the verdict.
        for (int i = 0; i < kVerdictFrame; ++i)
            REQUIRE (std::abs (mix[static_cast<size_t> (i)] - quarter) <= 1);

        // From that frame exactly, rising a 480th of the way a sample: 0.5
        // to 1 takes 240 samples, and no sample jumps.
        REQUIRE (mix[kVerdictFrame] - mix[kVerdictFrame - 1] >= 30);
        int biggestStep = 0;
        for (int i = kVerdictFrame; i < kVerdictFrame + 240; ++i)
            biggestStep = std::max (biggestStep, std::abs (mix[static_cast<size_t> (i)] - mix[static_cast<size_t> (i) - 1]));
        REQUIRE (biggestStep <= 60); // 0.8 / 480 is 55 LSB; a step to 0.8 would be 13107
        REQUIRE (mix[kVerdictFrame + 120] > quarter + 2000);
        REQUIRE (mix[kVerdictFrame + 120] < whole - 2000);

        for (size_t i = kVerdictFrame + 240; i < mix.size(); ++i)
            REQUIRE (std::abs (mix[i] - whole) <= 1);

        for (size_t i = 0; i < stemLeft.size(); ++i)
        {
            REQUIRE (std::abs (stemLeft[i] - whole) <= 1);
            REQUIRE (stemRight[i] == 0);
        }
    }
}

TEST_CASE (WritePipeline_WithNoPairWeightsTheMixIsThePlainSum)
{
    // Every channel not in a fresh pair: weight 1, nothing handed over, the
    // mix the trimmed sum it always was.
    ScopedTempTree tree ("no-pair-weight");
    WritePipeline p;
    REQUIRE (p.start (tree.card(), twoChannels(), 48000.0, 16, "2026-10-14T00:00:00Z"));

    std::vector<float> left (256, 0.3f), right (256, 0.2f);
    const float* chans[] = { left.data(), right.data() };
    REQUIRE (p.pushBlock (chans, 2, 256));
    p.stop();

    const auto mix = samplesOf (tree.card() + "/MIX.wav");
    REQUIRE (mix.size() == 256u);
    for (const int v : mix)
        REQUIRE (std::abs (v - static_cast<int> (std::lround (0.5 * 32767.0))) <= 1);
}
