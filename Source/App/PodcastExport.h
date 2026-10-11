#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../Core/StreamingTargets.h"
#include "../Core/TakeFolderTracker.h"
#include <functional>

namespace mma {

/// How one podcast-ready copy turned out.
struct PodcastExportResult
{
    /// True only once the copy is complete on disk under its final name.
    bool written = false;

    /// The finished copy. Unset unless `written`.
    juce::File output;

    /// The mix as it was measured, and what was done to it.
    double measuredLufs = 0.0;
    double measuredTruePeakDbtp = 0.0;
    double gainDb = 0.0;

    /// True when reaching the target would have meant limiting much of the
    /// take hard, so the copy fell back to the gain the peaks allow unlimited
    /// (adviseForTarget's) and is quieter than the target.
    bool limitedByTruePeak = false;

    /// The deepest the peak limiter turned anything down, in dB. Zero when no
    /// peak needed it.
    double peakLimitingDb = 0.0;

    /// The copy as measured on its way to disk: the target, or short of it
    /// after a fallback.
    double resultingLufs = 0.0;
    double resultingTruePeakDbtp = 0.0;

    /// One sentence in the app's voice (§10.6): what was saved, or why
    /// nothing was. Never empty.
    juce::String message;
};

/// "MIX - Apple-Podcasts.wav" for "MIX.wav" and "Apple Podcasts". The target's
/// name goes through SessionFolderNaming, because it ends up in a file name on
/// whatever card the take is on, and "Broadcast (EBU R128)" is not a name every
/// filesystem will take.
juce::String podcastCopyFileName (const juce::String& mixFileName, const std::string& targetName);

/// Writes a loudness-normalised copy of a finished take's mix beside it.
///
/// Reads `mixFileName` in `sessionFolder` -- and MIX_001.wav onwards, when a
/// long take split -- measures it with LoudnessMeter, applies the gain that
/// reaches monoTargetLufs(), and runs a look-ahead peak limiter (4x
/// oversampled, aiming 0.5 dB under the platform's true-peak ceiling) so the
/// transients that gain pushes over are turned down rather than clipped. The
/// result is one mono 24-bit WAV at the same rate. The take's own files are
/// only ever read.
///
/// A take so peaky that reaching the target would mean limiting a good part
/// of it by more than 10 dB is not squashed to get there: it gets the gain
/// adviseForTarget() offers instead (capped so its peaks fit unlimited), and
/// the message says it is quieter than the target and why.
///
/// Nothing is written for a take too short or too quiet to measure: a copy
/// "normalised" from a meaningless figure would be the one file in the folder
/// that is wrong on purpose. The result says why instead.
///
/// The copy is written under a working name and renamed into place once
/// complete, so a copy cut short by `cancel`, a full card or a crash never
/// sits in the folder looking like a finished one.
///
/// `currentFolder`, when given, is asked where the take folder is each time a
/// file in it is about to be opened, checked or renamed. The folder can be
/// renamed in Finder during the minutes a long copy takes; with this the copy
/// follows it rather than failing in -- and leaving its working file behind
/// for -- a folder that is no longer there. Without it, `sessionFolder`.
///
/// Blocking, and minutes of reading for a long take: never call it on the
/// audio or message thread. PodcastExporter is how the app runs it.
PodcastExportResult exportPodcastCopy (const juce::File& sessionFolder,
                                       const juce::String& mixFileName,
                                       const StreamingTarget& target,
                                       const std::atomic<bool>& cancel,
                                       const std::function<juce::File()>& currentFolder = {});

/// Runs exportPodcastCopy() after each take, on a thread of its own.
///
/// Stop must never wait on this: it is started once the take's files are
/// closed, and returns immediately. Takes stopped while one is still being
/// exported queue behind it rather than racing it for the disk the next take
/// is recording to.
class PodcastExporter
{
public:
    PodcastExporter();

    /// Cancels, and waits a short bounded while for the worker to clear away
    /// its unfinished copy. Never joins: a card that stopped answering can
    /// hold a read forever, and quitting must not wait on that.
    ~PodcastExporter();

    /// Queues one take. False when quitting, or when `targetName` names no
    /// target. `followedFolder`, when given, holds the take folder itself
    /// (TakeFolderTracker::duplicateDescriptor()), so the copy follows the
    /// folder through a rename in Finder while it waits or runs.
    bool start (const juce::File& sessionFolder, const juce::String& mixFileName,
                const std::string& targetName,
                std::shared_ptr<TakeFolderTracker> followedFolder = nullptr);

    /// Results finished since the last call, oldest first. For the message
    /// thread, which is the only place that may report them.
    std::vector<PodcastExportResult> collectFinished();

    bool isRunning() const;

    /// Stops the worker at its next block and refuses further work. Returns
    /// immediately.
    void cancel() noexcept;

    /// Waits up to `timeoutMs` for the worker to finish. True once idle.
    bool waitUntilIdle (int timeoutMs);

    /// The longest the destructor (and so quitting) will wait.
    static constexpr int kShutdownWaitMs = 2000;

private:
    struct Job
    {
        juce::File sessionFolder;
        juce::String mixFileName;
        StreamingTarget target;
        std::shared_ptr<TakeFolderTracker> followedFolder;
    };

    struct State
    {
        mutable std::mutex lock;
        std::condition_variable idle;
        std::deque<Job> queued;
        std::vector<PodcastExportResult> finished;
        bool running = false;
        std::atomic<bool> cancelling { false };
    };

    // Shared with the detached worker, which may outlive this object.
    std::shared_ptr<State> state;

    static void run (std::shared_ptr<State> state);
};

} // namespace mma
