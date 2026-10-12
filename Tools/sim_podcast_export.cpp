#include "App/PodcastExport.h"
#include "Core/LoudnessMeter.h"
#include "Core/SessionWriter.h"
#include "Core/StreamingTargets.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

// Headless check of the podcast-ready copy written after a take.
//
// The mixes here are written by SessionWriter -- the writer the app records
// MIX.wav with -- so what is exported is the file a real take leaves behind,
// not one shaped to suit the exporter. Each copy is then read back and measured
// again with LoudnessMeter, because the claim being checked is what the file
// measures, not what the exporter says it did.

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

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

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

/// Writes `seconds` of `sample(i)` as a take's mix, the way the app does.
bool writeMix (const juce::File& folder, double seconds, const std::function<float (int)>& sample,
               uint64_t splitBytes = 0, int* partsWritten = nullptr)
{
    mma::SessionWriter writer;
    if (splitBytes > 0)
        mma::SessionWriterTestAccess::useSplitSize (writer, splitBytes);

    if (! writer.open (folder.getChildFile ("MIX").getFullPathName().toStdString(),
                       kRate, 1, 24, "2026-10-05T12:00:00Z"))
        return false;

    const int total = static_cast<int> (seconds * kRate);
    std::vector<float> block (4800);

    for (int start = 0; start < total; start += static_cast<int> (block.size()))
    {
        const int count = std::min (static_cast<int> (block.size()), total - start);
        for (int i = 0; i < count; ++i)
            block[static_cast<size_t> (i)] = sample (start + i);

        if (! writer.writeInterleaved (block.data(), static_cast<size_t> (count)))
            return false;
    }

    if (partsWritten != nullptr)
        *partsWritten = writer.getSplitFileCount() + 1;

    return writer.close();
}

struct Measured
{
    bool ok = false;
    double rate = 0.0;
    int bits = 0;
    int channels = 0;
    juce::int64 samples = 0;
    double lufs = 0.0;
    double truePeak = 0.0;
};

Measured measure (const juce::File& file)
{
    Measured m;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
        return m;

    m.rate = reader->sampleRate;
    m.bits = static_cast<int> (reader->bitsPerSample);
    m.channels = static_cast<int> (reader->numChannels);
    m.samples = reader->lengthInSamples;

    mma::LoudnessMeter meter (reader->sampleRate);
    juce::AudioBuffer<float> buffer (static_cast<int> (reader->numChannels), 8192);

    for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += 8192)
    {
        const auto count = static_cast<int> (juce::jmin<juce::int64> (8192, reader->lengthInSamples - pos));
        if (! reader->read (&buffer, 0, count, pos, true, true))
            return m;

        meter.process (buffer.getReadPointer (0), static_cast<size_t> (count));
    }

    m.lufs = meter.getIntegratedLufs();
    m.truePeak = meter.getTruePeakDbtp();
    m.ok = true;
    return m;
}

/// A quiet voice-band tone with a slow swell, well under every target, so
/// every one of them wants it turned up and none is held back by its peaks.
float quietTone (int i)
{
    const double t = i / kRate;
    const double swell = 0.75 + 0.25 * std::sin (2.0 * kPi * 0.5 * t);
    return static_cast<float> (0.01 * swell * std::sin (2.0 * kPi * 440.0 * t));
}

int wavFilesIn (const juce::File& folder)
{
    return folder.getNumberOfChildFiles (juce::File::findFiles, "*.wav");
}

bool anyWorkingFileIn (const juce::File& folder)
{
    return folder.getNumberOfChildFiles (juce::File::findFiles, "*.part") > 0;
}

} // namespace

int main()
{
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getNonexistentChildFile ("sobstage-podcast-export", {}, false);

    if (! root.createDirectory().wasOk())
    {
        std::fprintf (stderr, "FAIL  could not create the temporary take folder\n");
        return 1;
    }

    struct Cleanup
    {
        juce::File root;
        ~Cleanup() { root.deleteRecursively(); }
    } cleanup { root };

    const std::atomic<bool> notCancelled { false };

    // 1. A quiet mono take, exported for every target there is: each copy must
    //    measure within 1 LU of the target's MONO figure, with its true peak
    //    under the ceiling, at the take's rate, 24-bit, mono, full length.
    {
        const auto take = root.getChildFile ("quiet-take");
        take.createDirectory();
        expect (writeMix (take, 12.0, quietTone), "the quiet take's mix could not be written");

        const auto source = measure (take.getChildFile ("MIX.wav"));
        expect (source.ok && source.lufs < -35.0, "the quiet take is not as quiet as the check needs");

        for (const auto& target : mma::streamingTargets())
        {
            const auto result = mma::exportPodcastCopy (take, "MIX.wav", target, notCancelled);
            const auto expectedName = mma::podcastCopyFileName ("MIX.wav", target.name);
            const auto copy = measure (result.output);
            const double wanted = mma::monoTargetLufs (target);

            std::printf ("  %-22s %-36s %6.2f LUFS (aim %6.2f), %6.2f dBTP (ceiling %.1f), gain %+.2f dB\n",
                         target.name.c_str(), result.output.getFileName().toRawUTF8(),
                         copy.lufs, wanted, copy.truePeak, target.truePeakCeilingDbtp, result.gainDb);

            expect (result.written, "a quiet take was not exported");
            expect (result.output.getFileName() == expectedName, "the copy is not named after the mix and target");
            expect (result.message.startsWith ("Podcast-ready copy saved: " + expectedName),
                    "the copy's message does not name it");
            expect (copy.ok, "the copy could not be read back");
            expect (std::abs (copy.lufs - wanted) <= 1.0, "the copy is not within 1 LU of the mono target");
            expect (copy.truePeak <= target.truePeakCeilingDbtp, "the copy's true peak is over the ceiling");
            expect (copy.rate == kRate, "the copy is not at the take's rate");
            expect (copy.bits == 24, "the copy is not 24-bit");
            expect (copy.channels == 1, "the copy is not mono");
            expect (copy.samples == source.samples, "the copy is not the whole take");
        }

        // The take's own file is only read.
        const auto after = measure (take.getChildFile ("MIX.wav"));
        expect (after.ok && std::abs (after.lufs - source.lufs) < 0.01, "exporting changed the take's own mix");
        expect (! anyWorkingFileIn (take), "an export left its working file behind");

        // A second export for the same target never overwrites the first.
        const auto* apple = mma::findStreamingTarget ("Apple Podcasts");
        expect (apple != nullptr, "there is no Apple Podcasts target");

        if (apple != nullptr)
        {
            const auto again = mma::exportPodcastCopy (take, "MIX.wav", *apple, notCancelled);
            expect (again.written && again.output.getFileName() == "MIX - Apple-Podcasts_2.wav",
                    "a second export overwrote the first instead of taking the next name");
            expect (again.message.contains ("(-19 LUFS)"), "Apple Podcasts' copy does not say -19 LUFS");
        }
    }

    const auto& apple = *mma::findStreamingTarget ("Apple Podcasts");

    // 2. Transient-heavy takes: a quiet tone with a full-scale click every
    //    second, and something closer to a voice -- noise in syllables with a
    //    1 ms plosive burst every 370 ms. Turning either up to the target
    //    pushes the transients far past the ceiling; the limiter must take
    //    them down so the copy still lands on the target, under the ceiling.
    auto checkLimitedTake = [&] (const char* name, const std::function<float (int)>& sample)
    {
        const auto take = root.getChildFile (name);
        take.createDirectory();
        expect (writeMix (take, 12.0, sample), "a transient-heavy take's mix could not be written");

        const auto source = measure (take.getChildFile ("MIX.wav"));
        const auto result = mma::exportPodcastCopy (take, "MIX.wav", apple, notCancelled);
        const auto copy = measure (result.output);
        const double wanted = mma::monoTargetLufs (apple);

        std::printf ("  %s: from %.2f LUFS / %.2f dBTP to %.2f LUFS / %.2f dBTP, limiting %.1f dB: %s\n",
                     name, source.lufs, source.truePeak, copy.lufs, copy.truePeak,
                     result.peakLimitingDb, result.message.toRawUTF8());

        expect (result.written, "a transient-heavy take was not exported");
        expect (! result.limitedByTruePeak, "a transient-heavy take fell back instead of being limited");
        expect (result.peakLimitingDb > 1.0, "the limiter did not touch a transient-heavy take");
        expect (std::abs (copy.lufs - wanted) <= 1.0, "a transient-heavy copy is not within 1 LU of the target");
        expect (std::abs (result.resultingLufs - copy.lufs) < 0.05, "the reported loudness is not the file's");
        expect (copy.truePeak <= apple.truePeakCeilingDbtp, "a transient-heavy copy's true peak is over the ceiling");
        expect (copy.samples == source.samples, "a transient-heavy copy is not the whole take");
        expect (result.message.contains ("(-19 LUFS), with the loudest peaks turned down by up to"),
                "a transient-heavy copy's message does not say its peaks were limited");
    };

    checkLimitedTake ("clicky-take", [] (int i)
    {
        return (i % 48000) == 24000 ? 0.9f : quietTone (i);
    });

    checkLimitedTake ("voice-like-take", [] (int i)
    {
        // A fixed-seed generator per sample index, so the take is the same
        // every run.
        auto noise = [] (int n)
        {
            auto x = static_cast<uint32_t> (n) * 2654435761u + 0x9e3779b9u;
            x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
            return static_cast<float> (x & 0xffff) / 32768.0f - 1.0f;
        };

        const double t = i / kRate;
        const double syllable = std::max (0.0, std::sin (2.0 * kPi * 4.0 * t));
        const bool plosive = (i % 17760) < 48; // 1 ms every 370 ms
        return plosive ? 0.5f * noise (i)
                       : static_cast<float> (0.08 * syllable) * noise (i);
    });

    // 3. A take that is transients through and through -- a click every 50 ms
    //    over a quiet tone. Reaching the target would mean limiting every
    //    moment of it by tens of dB, so the copy falls back to the gain its
    //    peaks allow unlimited, stays under the ceiling, and says it is short.
    {
        const auto take = root.getChildFile ("sustained-peaks-take");
        take.createDirectory();
        expect (writeMix (take, 12.0, [] (int i)
                {
                    return (i % 2400) == 1200 ? 0.9f : quietTone (i);
                }),
                "the sustained-peaks take's mix could not be written");

        const auto result = mma::exportPodcastCopy (take, "MIX.wav", apple, notCancelled);
        const auto copy = measure (result.output);

        std::printf ("  sustained peaks: %6.2f LUFS, %6.2f dBTP, fallback %d: %s\n",
                     copy.lufs, copy.truePeak, result.limitedByTruePeak ? 1 : 0,
                     result.message.toRawUTF8());

        expect (result.written, "the sustained-peaks take was not exported");
        expect (result.limitedByTruePeak, "the sustained-peaks take was squashed instead of falling back");
        expect (copy.truePeak <= apple.truePeakCeilingDbtp, "the sustained-peaks copy's true peak is over the ceiling");
        expect (copy.lufs < mma::monoTargetLufs (apple) - 1.0, "the sustained-peaks copy claims a loudness its peaks forbid");
        expect (result.message.contains ("quieter than Apple Podcasts wants"),
                "the sustained-peaks copy does not say it fell short of the target");
    }

    // 4. Silence: nothing to normalise, so nothing written, and the reason said.
    {
        const auto take = root.getChildFile ("silent-take");
        take.createDirectory();
        expect (writeMix (take, 10.0, [] (int) { return 0.0f; }), "the silent take's mix could not be written");

        const auto result = mma::exportPodcastCopy (take, "MIX.wav", apple, notCancelled);
        std::printf ("  silent take: %s\n", result.message.toRawUTF8());

        expect (! result.written, "a silent take was exported");
        expect (result.message.contains ("silent"), "a silent take's skip does not say it was silent");
        expect (wavFilesIn (take) == 1 && ! anyWorkingFileIn (take), "a silent take left a copy behind");
    }

    // 5. Too short to judge (one second): skipped, not given a meaningless gain.
    {
        const auto take = root.getChildFile ("short-take");
        take.createDirectory();
        expect (writeMix (take, 1.0, quietTone), "the short take's mix could not be written");

        const auto result = mma::exportPodcastCopy (take, "MIX.wav", apple, notCancelled);
        std::printf ("  short take: %s\n", result.message.toRawUTF8());

        expect (! result.written, "a one-second take was exported");
        expect (result.message.contains ("too short"), "a short take's skip does not say it was too short");
        expect (wavFilesIn (take) == 1, "a short take left a copy behind");
    }

    // 6. No mix at all.
    {
        const auto take = root.getChildFile ("empty-take");
        take.createDirectory();

        const auto result = mma::exportPodcastCopy (take, "MIX.wav", apple, notCancelled);
        expect (! result.written && result.message.contains ("MIX.wav"), "a missing mix was not named");
    }

    // 7. A long take that split into MIX_001.wav onwards is exported whole.
    {
        const auto take = root.getChildFile ("split-take");
        take.createDirectory();
        int parts = 0;
        expect (writeMix (take, 10.0, quietTone, 400000, &parts), "the split take's mix could not be written");
        expect (parts >= 3, "the mix did not split the way the check needs it to");

        const auto result = mma::exportPodcastCopy (take, "MIX.wav", apple, notCancelled);
        const auto copy = measure (result.output);

        std::printf ("  split take: %d parts, copy %.2f s\n", parts, static_cast<double> (copy.samples) / kRate);

        expect (result.written, "the split take was not exported");
        expect (copy.samples == static_cast<juce::int64> (10.0 * kRate), "the split take's copy is not the whole take");
    }

    // 8. Cancelled: nothing written, and no working file left behind.
    {
        const auto take = root.getChildFile ("cancelled-take");
        take.createDirectory();
        expect (writeMix (take, 10.0, quietTone), "the cancelled take's mix could not be written");

        const std::atomic<bool> cancelled { true };
        const auto result = mma::exportPodcastCopy (take, "MIX.wav", apple, cancelled);

        expect (! result.written, "a cancelled export wrote a copy");
        expect (wavFilesIn (take) == 1 && ! anyWorkingFileIn (take), "a cancelled export left a file behind");
    }

    // 9. The app's worker: two takes queued back to back both finish and are
    //    reported once each; once cancelled it takes nothing more, and its
    //    destruction is bounded.
    {
        const auto first = root.getChildFile ("worker-take-1");
        const auto second = root.getChildFile ("worker-take-2");
        first.createDirectory();
        second.createDirectory();
        expect (writeMix (first, 6.0, quietTone), "the first worker take could not be written");
        expect (writeMix (second, 6.0, quietTone), "the second worker take could not be written");

        mma::PodcastExporter exporter;
        expect (exporter.start (first, "MIX.wav", "Apple Podcasts"), "the worker did not take the first take");
        expect (exporter.start (second, "MIX.wav", "Apple Podcasts"), "the worker did not queue the second take");
        expect (! exporter.start (second, "MIX.wav", "No Such Platform"), "the worker took an unknown target");
        expect (exporter.waitUntilIdle (20000), "the worker did not finish");

        const auto finished = exporter.collectFinished();
        expect (finished.size() == 2 && finished[0].written && finished[1].written,
                "the worker did not report both takes as saved");
        expect (exporter.collectFinished().empty(), "the worker reported a take twice");
        expect (first.getChildFile ("MIX - Apple-Podcasts.wav").existsAsFile()
                    && second.getChildFile ("MIX - Apple-Podcasts.wav").existsAsFile(),
                "the worker's copies are not in their take folders");

        exporter.cancel();
        expect (! exporter.start (first, "MIX.wav", "Spotify"), "a cancelled worker took more work");
    }

    // A folder with files open in it cannot be renamed on Windows, and only a
    // Mac or Linux can hold the folder itself to follow it (TakeFolderTracker).
   #if defined (__APPLE__) || defined (__linux__)
    // 10. The take folder renamed in Finder while the copy is being made, at
    //     every point the copy looks for it: before the mix is measured,
    //     between passes, just before the finished copy is named. The copy
    //     ends up in the folder as it is now. It used to fail on the old path
    //     and leave its ".part" in the renamed folder, which the next launch
    //     deleted as unfinished work. A take with clicks in it, so the copy
    //     is written more than once on its way to the target.
    {
        const auto clicky = [] (int i) { return (i % 48000) == 24000 ? 0.9f : quietTone (i); };
        int lookups = 0;
        {
            const auto take = root.getChildFile ("count-lookups");
            take.createDirectory();
            expect (writeMix (take, 6.0, clicky), "the lookup-counting take could not be written");

            const auto counted = mma::exportPodcastCopy (take, "MIX.wav", apple, notCancelled,
                                                         [&lookups, take] { ++lookups; return take; });
            expect (counted.written, "the lookup-counting take was not exported");
        }

        // Listing, measuring, naming, then at least two passes of three looks
        // each (open, read, check), then the final name.
        expect (lookups >= 11, "the copy does not look for its folder before each step of each pass");
        std::printf ("  renamed mid-copy: the folder is looked for %d times; renaming at each\n", lookups);

        int followedEveryRename = 0;

        for (int renameAt = 1; renameAt <= lookups; ++renameAt)
        {
            const auto take = root.getChildFile ("rename-" + juce::String (renameAt));
            const auto renamed = root.getChildFile ("renamed-" + juce::String (renameAt) + " (keep)");
            take.createDirectory();

            if (! writeMix (take, 6.0, clicky))
            {
                expect (false, "a take to rename could not be written");
                continue;
            }

            int asked = 0;
            const auto result = mma::exportPodcastCopy (take, "MIX.wav", apple, notCancelled,
                                                        [&asked, renameAt, take, renamed]
            {
                if (++asked == renameAt)
                    take.moveFileTo (renamed);

                return asked >= renameAt ? renamed : take;
            });

            const bool followed = result.written
                               && result.output == renamed.getChildFile ("MIX - Apple-Podcasts.wav")
                               && result.output.existsAsFile()
                               && ! anyWorkingFileIn (renamed)
                               && measure (result.output).samples == static_cast<juce::int64> (6.0 * kRate);

            if (! followed)
                std::fprintf (stderr, "  renamed at look %d: %s\n", renameAt, result.message.toRawUTF8());

            followedEveryRename += followed ? 1 : 0;
        }

        expect (followedEveryRename == lookups, "a copy whose take folder was renamed did not follow it");
    }

    // 11. The app's worker, given the take folder itself: a take renamed while
    //     its copy waits behind another take's is still copied, into the
    //     renamed folder.
    {
        const auto ahead = root.getChildFile ("worker-ahead");
        const auto waiting = root.getChildFile ("2026-10-11_2330_Waiting");
        const auto renamed = root.getChildFile ("Waiting (keep)");
        ahead.createDirectory();
        waiting.createDirectory();
        expect (writeMix (ahead, 30.0, quietTone), "the take ahead could not be written");
        expect (writeMix (waiting, 6.0, quietTone), "the waiting take could not be written");

        auto followed = std::make_shared<mma::TakeFolderTracker>();
        expect (followed->open (waiting.getFullPathName().toStdString()), "could not hold the waiting take's folder");

        mma::PodcastExporter exporter;
        expect (exporter.start (ahead, "MIX.wav", "Apple Podcasts"), "the worker did not take the take ahead");
        expect (exporter.start (waiting, "MIX.wav", "Apple Podcasts", std::move (followed)),
                "the worker did not queue the waiting take");
        expect (waiting.moveFileTo (renamed), "could not rename the waiting take's folder");
        expect (exporter.waitUntilIdle (60000), "the worker did not finish");

        const auto finished = exporter.collectFinished();
        expect (finished.size() == 2 && finished[1].written
                    && finished[1].output == renamed.getChildFile ("MIX - Apple-Podcasts.wav")
                    && finished[1].output.existsAsFile(),
                "a take renamed while its copy waited did not get its copy in the renamed folder");
        expect (! anyWorkingFileIn (renamed), "the renamed take was left holding a working file");
    }
   #endif

    std::printf ("%s (%d checks, %d failing)\n",
                 failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED", checks, failures);
    return failures == 0 ? 0 : 1;
}
