#pragma once

#include "VolumeFingerprint.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mma {

/// The filesystem facts the UI needs while a take is live. Sampling them can
/// block on a slow or disappearing card, so this object owns the only worker
/// allowed to ask. Callers submit cheap in-memory requests and read snapshots.
class FilesystemStatusProbe
{
public:
    struct Request
    {
        std::string destinationPath;
        std::string sessionFolder;
        std::string mirrorPath;
        double bytesPerSecond = 0.0;
        /// §6.3 backup folder (it need not exist yet), compared against the
        /// destination to say whether the two share a disk. Empty: not asked.
        std::string backupRootPath;
        /// What a running backup writes per second. Counted against the
        /// destination's free space only when the backup shares its disk.
        double backupBytesPerSecond = 0.0;

        bool operator== (const Request& other) const noexcept
        {
            return destinationPath == other.destinationPath
                && sessionFolder == other.sessionFolder
                && mirrorPath == other.mirrorPath
                && bytesPerSecond == other.bytesPerSecond
                && backupRootPath == other.backupRootPath
                && backupBytesPerSecond == other.backupBytesPerSecond;
        }
        bool operator!= (const Request& other) const noexcept { return ! (*this == other); }
    };

    struct File
    {
        std::string name;
        int64_t sizeBytes = 0;
    };

    struct Snapshot
    {
        Request request;
        bool ready = false;
        /// True only when the requested session folder was inspected without a
        /// filesystem error. A completed worker cycle can still lack this
        /// evidence (for example, while a removable card is disappearing).
        bool filesObservationReady = false;
        double remainingSeconds = -1.0;
        int64_t mirrorFreeBytes = -1;
        /// 1 when the backup folder is on the destination's disk, 0 when it is
        /// known not to be, -1 when it was not asked or could not be told.
        int backupSharesDisk = -1;
        std::vector<File> files;
        /// Which volume is at the destination now (see VolumeFingerprint),
        /// asked between takes only -- with no session folder in the request.
        /// It is how a card swapped for another with the same name, or taken
        /// out, is noticed: the path stays the same and the drive check made
        /// for the old card must not arm the new one.
        VolumeFingerprint destinationVolume;
        /// When the sample behind this snapshot began, so a caller can tell
        /// evidence gathered after some event from evidence gathered before.
        std::chrono::steady_clock::time_point sampleStartedAt {};
        uint64_t revision = 0;

        // Diagnostic/test evidence that blocking filesystem calls did not run
        // on the caller that submitted the request.
        std::thread::id sampledOnThread;
    };

    explicit FilesystemStatusProbe (
        std::chrono::milliseconds refreshInterval = std::chrono::milliseconds (500));
    ~FilesystemStatusProbe();

    FilesystemStatusProbe (const FilesystemStatusProbe&) = delete;
    FilesystemStatusProbe& operator= (const FilesystemStatusProbe&) = delete;
    FilesystemStatusProbe (FilesystemStatusProbe&&) = delete;
    FilesystemStatusProbe& operator= (FilesystemStatusProbe&&) = delete;

    /// Changes what the worker samples. Repeating an identical request is a
    /// no-op; the worker refreshes the active request at its own bounded rate.
    void setRequest (Request request);
    Snapshot getSnapshot() const;

    /// Deterministic test/support hook. Shipping UI code never waits.
    bool waitForRevisionAfter (uint64_t revision, Snapshot& result,
                               std::chrono::milliseconds timeout) const;

    void stop();

private:
    struct State;
    using Sampler = std::function<Snapshot (const Request&)>;

    FilesystemStatusProbe (std::chrono::milliseconds refreshInterval,
                           Sampler sampler);

    static Snapshot sample (const Request& request);
    static void run (std::shared_ptr<State> state);
    static void launchWorker (const std::shared_ptr<State>& state);
    static bool sameFilesystemTargets (const Request& a, const Request& b) noexcept;

    mutable std::mutex stateHandleMutex;
    std::shared_ptr<State> state;

    friend struct FilesystemStatusProbeTestAccess;
};

} // namespace mma
