#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "../Core/CombinedTakePlan.h"
#include "../Core/TakeFolderTracker.h"

namespace mma {

/// Runs the combining step after a take, off the message thread.
///
/// Muxing a four-hour take is minutes of work even when the picture is only
/// copied, so none of it may happen on the thread drawing the meters. It also
/// must never be able to cost anyone a recording: every input file is already
/// closed and complete before this starts, nothing here writes to them, and a
/// failure leaves the take exactly as it was -- separate, complete, and
/// playable. The combined file is a convenience, and a convenience that can
/// take the take down with it is not one.
class TakeCombiner
{
public:
    TakeCombiner();
    ~TakeCombiner();

    /// How the last run ended, in the app's voice (§10.6).
    struct Status
    {
        bool running = false;
        int done = 0;
        int total = 0;
        juce::String problem;   ///< empty unless something went wrong
        juce::StringArray written; ///< file names of the combined files that exist

        /// Which start() this is the outcome of: different for every take
        /// started on its own (takes queued behind one share its run). A
        /// problem is reported once per run -- not once per wording, which
        /// silenced the second take that failed for the same reason.
        juce::uint64 run = 0;
    };

    /// Starts combining. Returns immediately; poll getStatus().
    ///
    /// A run already in flight is never doubled: two takes combining at once
    /// would fight for the same cores the next recording needs. The second
    /// take waits instead -- it is queued behind the first on the same worker,
    /// this returns true, and the run's status covers both. It used to return
    /// false and drop the plan, and the app never asked twice, so a take
    /// stopped while the last one was still muxing silently never got its
    /// combined file.
    ///
    /// `followedFolder`, when given, holds the take folder itself
    /// (TakeFolderTracker::duplicateDescriptor()), so a folder renamed in
    /// Finder while the combine waits or runs is followed: the inputs are
    /// read, and the combined file named, wherever the folder is by then.
    /// Without it the combine works in `sessionFolder` as given.
    bool start (const juce::File& sessionFolder, const CombinedTakePlan& plan,
                std::shared_ptr<TakeFolderTracker> followedFolder = nullptr);

    /// True from start() until the run has finished -- including while it is
    /// held for a take (below), so quitting and the recovery scan still wait.
    bool isRunning() const;
    Status getStatus() const;

    /// Held while a take is recording, and while its movies are still being
    /// finished: the card is the take's. Nothing starts while held, and a copy
    /// already under way is stopped, its unfinished file deleted, and made
    /// again from the start once released.
    ///
    /// The combine reads the whole movie and writes another of the same size
    /// to the same card. Run beside the next take it starved the take's writer
    /// -- the sound fell back to the mix alone, cameras dropped frames -- and
    /// filled the card under it. The Mac's ffmpeg once ran at the lowest disk
    /// priority for this; the Mac's own AVFoundation copy does its reading and
    /// writing on threads this app cannot lower, so waiting is the only way to
    /// be sure. Not a failure: no problem is reported for a held copy.
    ///
    /// Message thread. Idempotent; quitting (cancel()) still wins.
    void setHeldForRecording (bool held);

    /// Test seam: what availableBytesForRecording() would say about a folder.
    void setAvailableBytesForTesting (std::function<std::optional<uint64_t> (const std::string&)> query)
    {
        availableBytesQuery = std::move (query);
    }

    /// Test seam: how the finished file is given its name (File::moveFileTo).
    void setRenameForTesting (std::function<bool (const juce::File& from, const juce::File& to)> rename)
    {
        renameForTesting = std::move (rename);
    }

    /// How many times, and how far apart, the finished file's rename from its
    /// hidden working name is tried. Antivirus and indexing services open a
    /// file for a moment as it is closed, and Windows refuses a rename in
    /// that moment -- straight after ffmpeg exits, which is exactly when.
    static constexpr int kRenameAttempts = 20;
    static constexpr int kRenameRetryMs = 250;

    /// Requests that any ffmpeg child stop. Returns immediately; the detached
    /// worker owns its remaining cleanup and cannot refer back to this object.
    void cancel() noexcept;

    /// The ffmpeg this will use, or empty when none was found. A find is
    /// cached; a miss is not, because the user may install ffmpeg while the
    /// app runs -- the message says the next take will combine, so it must.
    /// Misses are re-checked at most every kFfmpegRetryMs here, since this is
    /// asked from the message thread and a PATH probe spawns a process;
    /// start() always looks again.
    juce::String findFfmpeg();

    static constexpr juce::uint32 kFfmpegRetryMs = 10000;

    /// What findFfmpeg() answers in the Mac app: the combine is done by the
    /// Mac's own AVFoundation, so there is nothing to install.
    static constexpr const char* kBuiltInCombiner = "builtin:avfoundation";

    /// Test seam: run this instead of looking for ffmpeg on the machine.
    void setFfmpegOverride (const juce::String& path) { ffmpegOverride = path; }

    /// Test seam: look in these places instead of ffmpegSearchPaths().
    void setFfmpegSearchPathsForTesting (std::vector<std::string> paths)
    {
        searchPathsForTesting = std::move (paths);
        hasSearchPathsForTesting = true;
    }

private:
    juce::String ffmpegOverride;
    std::vector<std::string> searchPathsForTesting;
    bool hasSearchPathsForTesting = false;
    juce::String resolvedFfmpeg;
    bool haveMissed = false;
    juce::uint32 lastMissMs = 0;
    juce::uint64 lastRun = 0;
    bool heldForRecording = false;
    std::function<std::optional<uint64_t> (const std::string&)> availableBytesQuery;
    std::function<bool (const juce::File&, const juce::File&)> renameForTesting;

    juce::String probeFfmpeg();

    struct QueuedTake
    {
        juce::File sessionFolder;
        CombinedTakePlan plan;
        std::shared_ptr<TakeFolderTracker> followedFolder;
    };

    struct RunState
    {
        std::atomic<bool> running { false };
        std::atomic<bool> cancelling { false };

        /// A take has the card (setHeldForRecording). Changed under statusLock
        /// and announced on `released`; read without it by a running copy.
        std::atomic<bool> held { false };
        std::condition_variable released;

        /// Free space where a combine would write, asked before each one.
        std::function<std::optional<uint64_t> (const std::string&)> availableBytes;

        /// Gives the finished file its name.
        std::function<bool (const juce::File&, const juce::File&)> rename;

        mutable std::mutex statusLock;
        Status status;

        /// Takes stopped while this run was busy, in the order they stopped.
        /// Guarded by statusLock, which is also held while the worker decides
        /// it has finished -- so a take is either picked up by this run or
        /// finds it over and starts its own. Never neither.
        std::deque<QueuedTake> queued;
    };

    std::shared_ptr<RunState> runState;

    static void run (std::shared_ptr<RunState> state,
                     juce::File sessionFolder,
                     CombinedTakePlan plan,
                     std::shared_ptr<TakeFolderTracker> followedFolder,
                     juce::String ffmpeg);

    static void combineTake (const std::shared_ptr<RunState>& state,
                             const juce::File& sessionFolder,
                             TakeFolderTracker* followedFolder,
                             const CombinedTakePlan& plan,
                             const juce::String& ffmpeg,
                             int& failures,
                             juce::String& firstFailureDetail);

    /// How one go at one camera's combine ended.
    enum class Attempt
    {
        Written,   ///< the combined file is there
        Failed,    ///< it isn't, and `problem` says why
        Yielded,   ///< stopped for a take that started; nothing left behind
        Cancelled  ///< quitting; nothing left behind
    };

    static Attempt combineOnce (RunState& state,
                                const juce::File& sessionFolder,
                                TakeFolderTracker* followedFolder,
                                const CombinedTakeJob& job,
                                const juce::String& ffmpeg,
                                juce::String& problem);

    /// Waits while a take has the card. False once quitting.
    static bool waitWhileHeld (RunState& state);
};

} // namespace mma
