#pragma once
#include "VolumeFingerprint.h"
#include <chrono>
#include <vector>
#include <string>
#include <cstdint>
#include <optional>

namespace mma {

struct PreflightResult
{
    bool passed = false;
    double sustainedMinBytesPerSec = 0.0;
    double requiredBytesPerSec = 0.0;
    std::string reason; // human-readable reason when !passed
    /// The card would not take the test file at all (read-only, full, gone).
    /// Whatever windows were measured before that are not a speed verdict.
    bool couldNotWrite = false;

    /// The drive's format can't hold one file over 4 GiB (FAT32 / MS-DOS).
    /// A property of the drive, so it is kept with the measurement.
    bool limitedTo4GiBFiles = false;

    /// Which volume was measured: read just before the test file was written.
    /// Unknown for a verdict that measured nothing (a check that could not be
    /// started), which is then never compared.
    VolumeFingerprint measuredVolume;

    /// When the verdict was reached, just after the test file was removed. A
    /// look at the drive that began before this is older evidence than the
    /// verdict itself, and is not weighed against it.
    std::chrono::steady_clock::time_point measuredVolumeConfirmedAt {};
};

/// §6.4 pre-flight throughput test. This class contains only the pure
/// calculations (required-rate math, rolling-minimum reduction, gate logic,
/// remaining-time formatting, cache-expiry check); actual file I/O to write
/// the 200MB test file belongs to the caller (SessionWriter/RecordingEngine),
/// keeping this class trivially testable without touching a real filesystem.
class PreflightThroughputTest
{
public:
    static constexpr size_t kTestFileBytes = 200ull * 1024ull * 1024ull;
    static constexpr double kRequiredMultiplier = 2.0; // must sustain >= 2x required rate
    static constexpr double kCacheExpiryDays = 30.0;

    /// Required sustained throughput per §6.4: channels * sampleRate * bytesPerSample * 2
    /// (card + mix file overhead), plus whatever the cameras are writing to the
    /// same card alongside it.
    ///
    /// The video is not a rounding error next to the audio: eight microphones at
    /// 24-bit/48k need about 4.6 MB/s, and one high-quality camera stream can
    /// ask for as much again. A card benchmarked against the audio alone can
    /// therefore pass this gate and still fail the moment a camera starts --
    /// which is precisely the mid-take degradation §6.4 exists to refuse in
    /// advance. Video is added once rather than doubled: the x2 above covers the
    /// stems plus the mix file, and there is only ever one copy of the video.
    static double requiredBytesPerSecond (int numChannels, double sampleRate, int bytesPerSample,
                                          double videoBytesPerSecond = 0.0) noexcept;

    /// Reduces a series of measured (rolling 1-second window) throughput samples,
    /// in bytes/sec, to the sustained minimum -- never the average.
    static double sustainedMinimum (const std::vector<double>& rollingWindowBytesPerSec) noexcept;

    /// Applies the pass/fail gate: sustained minimum must be >= 2x required.
    static PreflightResult evaluate (const std::vector<double>& rollingWindowBytesPerSec,
                                     int numChannels, double sampleRate, int bytesPerSample,
                                     double videoBytesPerSecond = 0.0);

    /// The same gate applied to a measurement already taken.
    ///
    /// How fast the card is belongs to the card; how fast it needs to be belongs
    /// to the take, and the take changes whenever a microphone or a camera is
    /// switched on. Keeping the two apart means the answer can be recomputed the
    /// moment someone reaches for record, instead of being frozen at whatever
    /// the rig happened to be when the 200 MB test last ran.
    static PreflightResult evaluateMeasured (double sustainedMinBytesPerSec,
                                             int numChannels, double sampleRate, int bytesPerSample,
                                             double videoBytesPerSecond = 0.0);

    /// The gate applied to a cached benchmark. A card that could not be
    /// written is refused as that, never as "too slow" -- with no windows it
    /// read 0 MB/s, and a partial run measured before the failure could even
    /// have passed.
    static PreflightResult evaluateCached (const PreflightResult& cached,
                                           int numChannels, double sampleRate, int bytesPerSample,
                                           double videoBytesPerSecond = 0.0);

    /// How long a "couldn't write" verdict is held before the card is tried
    /// again. Long enough not to hammer a missing volume every UI tick, short
    /// enough that freeing space or reinserting the card is noticed.
    static constexpr double kWriteFailureRetrySeconds = 10.0;

    /// True when a cached verdict should be thrown away and the benchmark run
    /// again. Only a write failure is transient in that way; a slow card stays
    /// slow until the user chooses the location again.
    static bool shouldRetryCached (const PreflightResult& cached, double secondsSinceVerdict,
                                   bool idle, bool workerRunning) noexcept;

    /// True when the drive now at the save location is not the one `cached`
    /// measured, so that verdict says nothing about it: the speed, the FAT32
    /// refusal, everything.
    ///
    /// `observed` is a fresh look at the path, begun at `observedFrom`. A look
    /// begun before the verdict confirmed its own volume is ignored -- a slow
    /// or stuck status worker must not keep throwing fresh verdicts away -- and
    /// unknown on either side is never a change.
    static bool measuredAnotherDrive (const PreflightResult& cached,
                                      const VolumeFingerprint& observed,
                                      std::chrono::steady_clock::time_point observedFrom) noexcept;

    /// Formats remaining free space as recording time in "Xh Ym" form, per §6.4
    /// ("remaining recording time in hours and minutes, not bytes").
    static std::string formatRemainingTime (uint64_t freeBytes, double requiredBytesPerSecPerFile) noexcept;

    /// True if a cached pass/fail result for this volume, recorded cacheAgeDays ago,
    /// has expired and preflight must be re-run.
    static bool isCacheExpired (double cacheAgeDays) noexcept { return cacheAgeDays >= kCacheExpiryDays; }

    enum class FilesystemKind { Unknown, ExFAT, NTFS, APFS, HFSPlus, FAT32, Other };

    /// True if the filesystem is one the OS cannot write large/long files to
    /// reliably (FAT32's 4GB file-size ceiling is exactly the case in §6.1/§6.4).
    static bool needsReformat (FilesystemKind kind) noexcept { return kind == FilesystemKind::FAT32; }

    /// The format from the name the OS gives it (statfs f_fstypename on the
    /// Mac: "msdos", "exfat", "apfs", "hfs", "ntfs"; "vfat"/"fat32" elsewhere).
    static FilesystemKind filesystemKindFromTypeName (const std::string& typeName) noexcept;

    /// Why a take can't arm on this drive, or empty. The WAVs split at 3.9 GB
    /// and survive FAT32; a camera movie is one file with no cap, and stops
    /// with an error about 18 minutes in when it reaches 4 GiB -- leaving the
    /// rest of the show with no video. So FAT32 refuses only with a camera on.
    static std::string fileSizeRefusal (bool limitedTo4GiBFiles, int enabledCameras);

    /// Why the drive check could not write, from the error the OS gave.
    /// A macOS privacy refusal ("Don't Allow" on files on a removable volume,
    /// Documents, Desktop) arrives as EPERM/EACCES and used to be reported as
    /// a card fault -- someone re-seated a card that was fine.
    ///
    /// `kind` is the drive's format where it is known. A read-only refusal on
    /// an NTFS drive is not a locked card: macOS mounts Windows-formatted disks
    /// read-only, and "slide its lock switch off" sent people hunting for a
    /// switch an external SSD does not have.
    static std::string writeFailureReason (int errnoValue,
                                           FilesystemKind kind = FilesystemKind::Unknown);
};

} // namespace mma
