#pragma once
#include <string>
#include <fstream>
#include <vector>
#include <cstdint>

namespace mma {

/// Whether the space left on a drive means it is full, asked as the writer
/// experiences it: there is no longer room for the audio one more second of
/// THIS take would need.
///
/// A fixed byte count would mean different things at 16 bits mono and 32 bits
/// across eight channels, and this take's own format is the only figure that is
/// about this take. Separated from the writer so both answers can be tested:
/// the interesting half of the full-drive fix needs a filesystem that is
/// genuinely out of space, which no unit test can conjure.
bool freeSpaceMeansDriveIsFull (unsigned long long bytesAvailable,
                                int bytesPerSample,
                                int numChannels,
                                double sampleRate) noexcept;

/// §6.1/§6.3/§6.6: a single BWF-tagged WAV (or RF64) file writer with
/// auto-split at 3.9GB and 5-second header rewrites for crash safety.
/// Real file I/O -- exercised by Tests/ against tmp files on this Linux
/// sandbox, since none of it needs real audio hardware.
class SessionWriter
{
public:
    static constexpr uint64_t kAutoSplitBytes = 3900ull * 1000ull * 1000ull; // 3.9 GB
    static constexpr double kHeaderRewriteIntervalSeconds = 5.0;

    SessionWriter() = default;
    ~SessionWriter();

    /// basePath is the file path WITHOUT the _NNN split suffix or extension, e.g.
    /// ".../01_Yeti-Kitchen"; ".wav" is appended, and "_001" etc. once a second
    /// file is needed. originTimestampIso is the shared BWF session-origin
    /// timestamp stamped into every stem and the mix file.
    bool open (const std::string& basePath, double sampleRate, int numChannels, int bitDepth,
              const std::string& originTimestampIso);

    /// Writes one block of interleaved float samples in [-1, 1]. Converts to the
    /// configured bit depth, auto-splitting to a new numbered file when the
    /// current file would exceed kAutoSplitBytes. No throwing; returns false on
    /// an unrecoverable write failure (caller handles per §6.5 "target card removed").
    bool writeInterleaved (const float* interleaved, size_t numFrames);

    /// Call periodically (e.g. from a timer, not the audio thread) with elapsed
    /// wall-clock seconds; rewrites the RIFF/data-chunk-size header fields every
    /// kHeaderRewriteIntervalSeconds so a crash mid-take leaves a playable file.
    /// Returns false when the periodic header rewrite failed, which is what a
    /// card pulled mid-take looks like from here. §6.6 rewrites the header
    /// every 5 seconds so an interrupted file stays playable; the result was
    /// discarded, so the write that keeps a four-hour take recoverable could
    /// start failing and nothing would notice until the stop.
    bool tick (double dtSeconds);

    /// Why the last write failed, in the user's words, or empty. §6.1 rolls to
    /// a new file at 3.9 GB, and a roll-over that fails is a different fault
    /// from a write that fails -- the take stops at a file boundary rather than
    /// mid-block, and "the card stopped accepting writes" does not describe it.
    const std::string& getWriteProblem() const noexcept { return writeProblem; }

    /// Whether the drive ran out of room, as distinct from the sentence about
    /// it. The sentence is written for the card -- "recording has stopped and
    /// every file has been closed" -- and the mirror's failure stops no
    /// recording at all, so lending it that wording would tell someone their
    /// take had ended when it had not. Each caller words its own sentence from
    /// this.
    bool ranOutOfSpace() const noexcept { return outOfSpace; }

    /// Finalizes the current file (writes a final correct header) and closes it.
    ///
    /// Returns false when that final header rewrite or the flush behind it
    /// failed -- which is exactly what pulling the card during a stop looks
    /// like. This used to return void, so the one write that decides whether a
    /// finished take is playable was the one write nobody checked: the app
    /// reported the take saved, and the file on the card carried a header
    /// claiming zero audio.
    bool close();

    uint64_t getTotalFramesWritten() const { return totalFramesWritten; }
    int getSplitFileCount() const { return splitIndex; }
    std::string getCurrentFilePath() const { return currentFilePath; }

    /// Where the file being written is NOW, asked of the open file rather than
    /// remembered: a take folder renamed or moved in Finder mid-take carries
    /// the file with it, and the path this writer opened no longer exists.
    /// F_GETPATH on macOS, /proc/self/fd on Linux. Empty when the system can't
    /// say (no file open, Windows, a file that has been deleted). Call only
    /// from the thread that drives this writer.
    std::string resolveCurrentFilePath() const;

private:
    std::string basePathNoExt;
    std::string originTimestamp;
    double sampleRate = 48000.0;
    int numChannels = 1;
    int bitDepth = 24;
    int splitIndex = 0; // 0 = no split suffix yet; becomes 1 ("_001") on first split

    std::fstream file;
    std::string currentFilePath;

    /// A second descriptor on the open file, held for the life of that file,
    /// that the periodic sync and the last-resort header patch go through.
    ///
    /// Both used to reopen the file BY PATH every time. The take's own stream
    /// follows the file wherever it goes, but a path does not: rename the take
    /// folder in Finder mid-take, or drag it somewhere else on the same card,
    /// and the very next 5-second sync failed to open a path that no longer
    /// existed. That failure reads as the card going away, so a healthy take
    /// was stopped under "the drive stopped responding" within five seconds.
    /// POSIX only; -1 when not open (and always on Windows, which will not
    /// rename a folder holding an open file in the first place).
    int syncDescriptor = -1;
    void closeSyncDescriptor() noexcept;
    std::string writeProblem;
    bool outOfSpace = false;
    uint64_t dataBytesWrittenToCurrentFile = 0;
    uint64_t totalFramesWritten = 0;
    uint64_t autoSplitBytes = kAutoSplitBytes;
    double secondsSinceLastHeaderRewrite = 0.0;

    // Byte offsets of header fields we rewrite in place.
    std::streampos riffSizeFieldPos {};
    std::streampos dataSizeFieldPos {};

    /// Names why a write failed, when the filesystem can be asked and gives a
    /// definite answer. Leaves writeProblem empty otherwise, so the caller's
    /// "the card stopped accepting writes" account stands -- which is the right
    /// one for a drive that has actually gone.
    void noteWriteFailureCause();

    std::string makePathForSplit (int index) const;
    bool openNewFile (int index);

    /// Moves basePathNoExt into the folder the open file is in now, when that
    /// is not the folder the take was opened in. The 3.9 GB roll-over builds
    /// the next file's name from basePathNoExt, and every other write follows
    /// a take folder renamed in Finder through the open file -- the roll-over
    /// alone went back to the old path, found no folder there, and stopped the
    /// take under "the card may be full".
    void followFolderOf (const std::string& liveFilePath);

    /// RIFF chunks occupy an even number of bytes: an odd data chunk is
    /// followed by one pad byte its size does not count. Appended only when a
    /// file is finished (close, or the rollover to the next split) -- never
    /// mid-take, where the next frame would land after it.
    bool appendPadByteIfOdd();
    void writeHeaderPlaceholder();
    /// False when the seek/write/flush behind the header patch failed.
    bool rewriteHeaderSizes();
    bool syncCurrentFileToStorage();

    /// Writes the RIFF and data sizes through a FRESH handle, describing what
    /// actually landed on disk rather than what the writer tried to send.
    ///
    /// The last resort when the take's own stream can no longer be patched,
    /// which is exactly what a full card produces. Needs no new space: it
    /// overwrites four bytes at two offsets that already exist.
    bool patchHeaderThroughFreshHandle();
    int bytesPerSample() const { return bitDepth / 8; }

    /// Makes the multi-file boundary reachable without writing 3.9 GB in a
    /// unit test. Production code never calls this and always keeps the public
    /// kAutoSplitBytes limit.
    void setAutoSplitBytesForTesting (uint64_t bytes) { autoSplitBytes = bytes; }

    friend struct SessionWriterTestAccess;
};

} // namespace mma
