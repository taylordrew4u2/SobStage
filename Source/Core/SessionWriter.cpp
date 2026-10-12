#include "SessionWriter.h"
#include "Utf8Path.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <system_error>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <fcntl.h>
 #include <sys/param.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace mma {

namespace {

void writeU32LE (std::ostream& out, uint32_t v)
{
    char b[4] = { static_cast<char> (v & 0xFF), static_cast<char> ((v >> 8) & 0xFF),
                  static_cast<char> ((v >> 16) & 0xFF), static_cast<char> ((v >> 24) & 0xFF) };
    out.write (b, 4);
}

void writeU16LE (std::ostream& out, uint16_t v)
{
    char b[2] = { static_cast<char> (v & 0xFF), static_cast<char> ((v >> 8) & 0xFF) };
    out.write (b, 2);
}

void writeTag (std::ostream& out, const char* tag) { out.write (tag, 4); }

} // namespace

SessionWriter::~SessionWriter()
{
    close();
}

std::string SessionWriter::makePathForSplit (int index) const
{
    if (index <= 0)
        return basePathNoExt + ".wav";

    char suffix[16];
    std::snprintf (suffix, sizeof (suffix), "_%03d", index);
    return basePathNoExt + suffix + ".wav";
}

bool SessionWriter::open (const std::string& basePath, double sampleRateIn, int numChannelsIn, int bitDepthIn,
                          const std::string& originTimestampIso)
{
    // Only the depths the packer below actually writes. 32 used to be
    // accepted here, given a 32-bit header, and then packed as 24-bit --
    // every file unreadable, no error anywhere.
    if (bitDepthIn != 16 && bitDepthIn != 24 && bitDepthIn != 32)
        return false;

    basePathNoExt = basePath;
    sampleRate = sampleRateIn;
    numChannels = numChannelsIn;
    bitDepth = bitDepthIn;
    originTimestamp = originTimestampIso;
    splitIndex = 0;
    writeProblem.clear();
    outOfSpace = false;
    totalFramesWritten = 0;
    secondsSinceLastHeaderRewrite = 0.0;

    return openNewFile (0);
}

void SessionWriter::closeSyncDescriptor() noexcept
{
#if ! defined (_WIN32)
    if (syncDescriptor >= 0)
        ::close (syncDescriptor);
#endif
    syncDescriptor = -1;
}

bool SessionWriter::openNewFile (int index)
{
    if (file.is_open())
        file.close();

    closeSyncDescriptor();

    currentFilePath = makePathForSplit (index);
    // UTF-8 on the way in; see Utf8Path.h for what a narrow open does on Windows.
    const auto nativePath = pathFromUtf8 (currentFilePath);
    file.open (nativePath, std::ios::binary | std::ios::out | std::ios::in | std::ios::trunc);
    if (! file.is_open())
    {
        // Retry with out-only (some platforms need the file to exist before in|out trunc works).
        file.open (nativePath, std::ios::binary | std::ios::out | std::ios::trunc);
        file.close();
        file.open (nativePath, std::ios::binary | std::ios::out | std::ios::in);
        if (! file.is_open())
            return false;
    }

    dataBytesWrittenToCurrentFile = 0;
    writeHeaderPlaceholder();

    // Flushed and checked, because creating the file proves almost nothing on
    // a card that is out of room: an empty file needs an entry, not a block,
    // so the open succeeds and the header -- which does need a block -- fails
    // quietly in the buffer.
    //
    // That was the whole of it. open() returned true, startRecording returned
    // true, and the app said it was recording onto a card that could not take
    // a single byte. Application.cpp already stops the engine and tells the
    // user when startRecording fails, with a comment saying that path "used to
    // return here in silence" -- it simply was never reached for a full card,
    // so someone hit record, performed, and only learned afterwards.
    //
    // A flush per file at open time, once per channel per take, costs nothing
    // worth measuring.
    file.flush();

    if (! file.good())
    {
        // Said here rather than left to the caller's generic sentence, for the
        // same reason as every other account in this file: "couldn't start
        // recording" is not something anyone can act on.
        outOfSpace = true;
        writeProblem = "There isn't enough room on the drive to start this take. Free up "
                       "space on it, or record to a bigger card, then try again.";
        file.close();

        // The empty file is removed rather than left in the take folder. A
        // zero-byte .wav sitting beside the others reads as a track that
        // recorded nothing, which is a different and more alarming thing than
        // a take that never started.
        std::error_code ignored;
        std::filesystem::remove (nativePath, ignored);

        return false;
    }

#if ! defined (_WIN32)
    // Opened now, while the path is certainly this file's, and kept: see the
    // member's comment. CLOEXEC because this process starts ffmpeg, which has
    // no business inheriting a handle on a take that is still being written.
    // A refusal here is not fatal -- the sync falls back to the path, which is
    // what it always did.
    syncDescriptor = ::open (currentFilePath.c_str(), O_RDWR | O_CLOEXEC);
#endif

    return true;
}

void SessionWriter::writeHeaderPlaceholder()
{
    // RIFF header.
    writeTag (file, "RIFF");
    riffSizeFieldPos = file.tellp();
    writeU32LE (file, 0); // patched later
    writeTag (file, "WAVE");

    // fmt chunk.
    writeTag (file, "fmt ");
    writeU32LE (file, 16);
    writeU16LE (file, 1); // PCM
    writeU16LE (file, static_cast<uint16_t> (numChannels));
    writeU32LE (file, static_cast<uint32_t> (sampleRate));
    const uint32_t byteRate = static_cast<uint32_t> (sampleRate) * static_cast<uint32_t> (numChannels) * static_cast<uint32_t> (bytesPerSample());
    writeU32LE (file, byteRate);
    writeU16LE (file, static_cast<uint16_t> (numChannels * bytesPerSample()));
    writeU16LE (file, static_cast<uint16_t> (bitDepth));

    // bext (Broadcast WAVE) chunk: minimal fixed-size 602-byte body, per EBU
    // Tech 3285, carrying the origination date/time so a DAW can align stems
    // without manual nudging (§6.1).
    writeTag (file, "bext");
    writeU32LE (file, 602);
    {
        std::vector<char> bext (602, 0);
        // Description[256], Originator[32], OriginatorReference[32] left blank/zero.
        // OriginationDate[10] "YYYY-MM-DD" at offset 320, OriginationTime[8] "HH:MM:SS" at offset 330.
        std::string date, time;
        if (originTimestamp.size() >= 19)
        {
            date = originTimestamp.substr (0, 10);
            time = originTimestamp.substr (11, 8);
        }
        std::memcpy (bext.data() + 320, date.data(), std::min<size_t> (date.size(), 10));
        std::memcpy (bext.data() + 330, time.data(), std::min<size_t> (time.size(), 8));
        // BWF TimeReferenceLow/High (offset 338/342) is the sample offset from
        // the shared session origin. The first file starts at zero; a split
        // continuation starts after every frame already written, so a DAW lays
        // the pieces end to end instead of stacking every _NNN file at t=0.
        const uint64_t timeReference = totalFramesWritten;
        for (int byte = 0; byte < 8; ++byte)
            bext[static_cast<size_t> (338 + byte)] =
                static_cast<char> ((timeReference >> (byte * 8)) & 0xff);
        // Version (offset 346) = 1.
        bext[346] = 1;
        file.write (bext.data(), static_cast<std::streamsize> (bext.size()));
    }

    // data chunk.
    writeTag (file, "data");
    dataSizeFieldPos = file.tellp();
    writeU32LE (file, 0); // patched as data is written and on close
}

bool freeSpaceMeansDriveIsFull (unsigned long long bytesAvailable,
                                int bytesPerSample,
                                int numChannels,
                                double sampleRate) noexcept
{
    const auto bytesPerSecond = static_cast<unsigned long long> (std::max (1, bytesPerSample))
                              * static_cast<unsigned long long> (std::max (1, numChannels))
                              * static_cast<unsigned long long> (std::max (1.0, sampleRate));

    return bytesAvailable < bytesPerSecond;
}

bool SessionWriter::writeInterleaved (const float* interleaved, size_t numFrames)
{
    if (! file.is_open())
        return false;

    const size_t bps = static_cast<size_t> (bytesPerSample());
    const size_t frameBytes = bps * static_cast<size_t> (numChannels);

    size_t frameStart = 0;
    while (frameStart < numFrames)
    {
        // Auto-split at 3.9GB (§6.1) before writing would push us over.
        if (dataBytesWrittenToCurrentFile + frameBytes > autoSplitBytes)
        {
            if (! appendPadByteIfOdd() || ! rewriteHeaderSizes())
            {
                writeProblem = "Couldn't finish " + currentFilePath
                             + " before starting the next file. Recording has stopped to protect the take.";
                return false;
            }

            // The file we are rolling over FROM, captured before openNewFile
            // reassigns currentFilePath to the one it is about to create --
            // and asked of the open file, so a take folder renamed since the
            // take began is named where it now is.
            auto previousFilePath = currentFilePath;

            // Asked BEFORE openNewFile, which closes the descriptor this
            // answer comes from.
            if (const auto live = resolveCurrentFilePath(); ! live.empty())
            {
                previousFilePath = live;
                followFolderOf (live);
            }

            splitIndex = std::max (1, splitIndex + 1);

            if (! openNewFile (splitIndex))
            {
                // The caller turns a false into "the card stopped accepting
                // writes", which is true but not what happened: the take ran
                // past 3.9 GB and the next file could not be created. Same
                // outcome, different thing to check.
                //
                // openNewFile sets currentFilePath to the NEW path before it
                // can fail, so this used to name the file that does not exist
                // as the one it came after. It also overwrote the more
                // specific out-of-space sentence openNewFile had just set, so
                // the vaguer message won whenever the drive was simply full.
                //
                // A folder that is not there is not a full card either: the
                // take's folder went somewhere the system could not follow
                // (dragged to another drive, say), and that is what to check.
                std::error_code ec;
                const auto folder = pathFromUtf8 (currentFilePath).parent_path();
                const bool folderIsThere = folder.empty() || std::filesystem::is_directory (folder, ec);

                if (writeProblem.empty() && ! folderIsThere)
                    writeProblem = "Couldn't start the next file after " + previousFilePath
                                 + ", because this take's folder can no longer be found. It may "
                                   "have been moved or deleted while recording. Recording has "
                                   "stopped to protect the take.";
                else if (writeProblem.empty())
                    writeProblem = "Couldn't start the next file after " + previousFilePath
                                 + ". The card may be full, or may not allow files this large.";

                return false;
            }
        }

        const float* frame = interleaved + frameStart * static_cast<size_t> (numChannels);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            // Non-finite input is written as silence: std::clamp passes NaN
            // straight through, and the integer conversion of NaN is unspecified.
            float s = std::isfinite (frame[ch]) ? std::clamp (frame[ch], -1.0f, 1.0f) : 0.0f;
            if (bitDepth == 16)
            {
                int16_t v = static_cast<int16_t> (std::lround (s * 32767.0f));
                char b[2] = { static_cast<char> (v & 0xFF), static_cast<char> ((v >> 8) & 0xFF) };
                file.write (b, 2);
            }
            else if (bitDepth == 32)
            {
                // Integer PCM, full scale. Computed in double: float cannot
                // hold 2^31 - 1 exactly and would round past it.
                const int32_t v = static_cast<int32_t> (std::llround (static_cast<double> (s) * 2147483647.0));
                const auto u = static_cast<uint32_t> (v);
                char b[4] = { static_cast<char> (u & 0xFF), static_cast<char> ((u >> 8) & 0xFF),
                              static_cast<char> ((u >> 16) & 0xFF), static_cast<char> ((u >> 24) & 0xFF) };
                file.write (b, 4);
            }
            else // 24-bit
            {
                int32_t v = static_cast<int32_t> (std::lround (s * 8388607.0f));
                char b[3] = { static_cast<char> (v & 0xFF), static_cast<char> ((v >> 8) & 0xFF),
                              static_cast<char> ((v >> 16) & 0xFF) };
                file.write (b, 3);
            }
        }

        dataBytesWrittenToCurrentFile += frameBytes;
        ++totalFramesWritten;
        ++frameStart;
    }

    if (! file.good())
    {
        noteWriteFailureCause();
        return false;
    }

    return true;
}

void SessionWriter::noteWriteFailureCause()
{
    // A more specific account already set by the split path wins: it knows
    // something this cannot work out from free space alone.
    if (! writeProblem.empty())
        return;

    std::error_code ec;
    const auto space = std::filesystem::space (
        pathFromUtf8 (currentFilePath).parent_path(), ec);

    if (ec)
        return;

    if (! freeSpaceMeansDriveIsFull (static_cast<unsigned long long> (space.available),
                                     bytesPerSample(), numChannels, sampleRate))
        return;

    // §10.6: what happened, then what to do. Without this the take stopped
    // under the card-removal notice, which tells the user the drive "stopped
    // responding" and to check that it is plugged in properly -- so someone
    // whose card is merely full spends the one moment they are still next to
    // the rig re-seating a cable that was never loose. The comment at that
    // branch in Application.cpp says exactly this; the account it looks for
    // was simply never written for an ordinary failed write, only for a
    // roll-over past 3.9 GB.
    outOfSpace = true;
    writeProblem = "The drive you were recording to is full, so recording has stopped and "
                   "every file has been closed. Free up space on it, or record to a bigger "
                   "card, then start a new take.";
}

bool SessionWriter::rewriteHeaderSizes()
{
    if (! file.is_open())
        return false;

    // A stream that has already failed silently discards every later seek and
    // write, so this patch did nothing on precisely the takes that needed it.
    //
    // That is what a full card does. The audio written before the drive gave
    // out is on the card and perfectly good, but the header still carries its
    // placeholder zeros -- RIFF size 0, data size 0 -- so every player and DAW
    // opens the file and sees a recording with nothing in it. Measured on a
    // real full filesystem: two of three stems held about 2.4 seconds of audio
    // each and both declared themselves empty, while the app said every file
    // had been closed.
    //
    // The comment on close() in the header names this as the failure it exists
    // to prevent -- "the file on the card carried a header claiming zero
    // audio" -- and the return value it added does report it. What was missing
    // is that the patch can actually succeed: it overwrites four bytes at two
    // offsets that already exist, so it needs no new space and a full drive
    // cannot refuse it. It only ever needed to be allowed to try.
    //
    // Clearing here cannot hide a genuine failure. Every path that writes
    // audio judges the stream itself, cardWriteFailed is already latched by
    // then, and the return below is taken after the patch -- so a device that
    // really has gone still fails, and still says so.
    file.clear();

    const auto currentPos = file.tellp();
    file.seekp (0, std::ios::end);
    const auto fileEnd = file.tellp();
    file.seekp (currentPos);

    const uint32_t dataSize = static_cast<uint32_t> (std::min<uint64_t> (dataBytesWrittenToCurrentFile, 0xFFFFFFFFull));
    const uint64_t totalFileBytes = static_cast<uint64_t> (fileEnd);
    const uint32_t riffSize = static_cast<uint32_t> (std::min<uint64_t> (totalFileBytes >= 8 ? totalFileBytes - 8 : 0, 0xFFFFFFFFull));

    file.seekp (riffSizeFieldPos);
    writeU32LE (file, riffSize);
    file.seekp (dataSizeFieldPos);
    writeU32LE (file, dataSize);

    file.seekp (currentPos);
    file.flush();

    return file.good() && syncCurrentFileToStorage();
}

bool SessionWriter::appendPadByteIfOdd()
{
    if ((dataBytesWrittenToCurrentFile & 1u) == 0)
        return true;

    file.clear();
    const auto currentPos = file.tellp();
    file.seekp (0, std::ios::end);
    file.put ('\0');
    file.seekp (currentPos);
    return file.good();
}

bool SessionWriter::syncCurrentFileToStorage()
{
    // std::fstream::flush() only reaches the operating-system cache. §6.6's
    // periodic header rewrite is specifically the crash/power-loss boundary,
    // so ask the OS to push those bytes to the device as well. This function is
    // called by the writer/timer path, never by an audio callback.
#if defined (_WIN32)
    // Wide, not CreateFileA: the A form reads the path in the ANSI code page.
    const HANDLE handle = CreateFileW (pathFromUtf8 (currentFilePath).c_str(), GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;

    const bool ok = FlushFileBuffers (handle) != 0;
    CloseHandle (handle);
    return ok;
#else
    // The descriptor held since the file was created, so the sync reaches this
    // file even when its folder has since been renamed or moved. The path is
    // only a fallback for a descriptor that could not be opened.
    const bool ownsDescriptor = syncDescriptor < 0;
    const int descriptor = ownsDescriptor ? ::open (currentFilePath.c_str(), O_RDWR | O_CLOEXEC)
                                          : syncDescriptor;
    if (descriptor < 0)
        return false;

    bool ok = false;
 #if defined (__APPLE__) && defined (F_FULLFSYNC)
    ok = ::fcntl (descriptor, F_FULLFSYNC) == 0;
    if (! ok)
        ok = ::fsync (descriptor) == 0;
 #else
    ok = ::fsync (descriptor) == 0;
 #endif
    if (ownsDescriptor)
        ::close (descriptor);
    return ok;
#endif
}

std::string SessionWriter::resolveCurrentFilePath() const
{
#if defined (__APPLE__) && defined (F_GETPATH)
    if (syncDescriptor < 0)
        return {};

    char buffer[MAXPATHLEN] = {};
    if (::fcntl (syncDescriptor, F_GETPATH, buffer) != 0)
        return {};

    return std::string (buffer);
#elif defined (__linux__)
    if (syncDescriptor < 0)
        return {};

    const auto link = "/proc/self/fd/" + std::to_string (syncDescriptor);
    char buffer[4096] = {};
    const auto length = ::readlink (link.c_str(), buffer, sizeof (buffer) - 1);
    if (length <= 0)
        return {};

    std::string path (buffer, static_cast<size_t> (length));

    // The kernel's way of saying the file has no name any more.
    const std::string deleted = " (deleted)";
    if (path.size() >= deleted.size()
        && path.compare (path.size() - deleted.size(), deleted.size(), deleted) == 0)
        return {};

    return path;
#else
    return {};
#endif
}

void SessionWriter::followFolderOf (const std::string& liveFilePath)
{
    const auto liveFolder = pathFromUtf8 (liveFilePath).parent_path();
    if (liveFolder.empty())
        return;

    const auto base = pathFromUtf8 (basePathNoExt);
    const auto openedFolder = base.has_parent_path() ? base.parent_path()
                                                     : std::filesystem::path (".");

    // Compared as folders, not as strings: the system answers with the
    // resolved path, and /tmp on a Mac is /private/tmp. A folder that has not
    // moved keeps the spelling the take was given. An error -- which is what
    // asking about a folder that no longer exists gives -- counts as moved.
    std::error_code ec;
    const bool unmoved = std::filesystem::equivalent (openedFolder, liveFolder, ec) && ! ec;
    if (unmoved)
        return;

    // Only the folder follows. The name stays the one this take gave it, so a
    // file the user renamed by hand still gets its _NNN continuation under the
    // track's own name, where the take combiner looks for it.
    basePathNoExt = utf8FromPath (liveFolder / base.filename());
}

bool SessionWriter::tick (double dtSeconds)
{
    secondsSinceLastHeaderRewrite += dtSeconds;

    if (secondsSinceLastHeaderRewrite < kHeaderRewriteIntervalSeconds)
        return true;

    secondsSinceLastHeaderRewrite = 0.0;
    return rewriteHeaderSizes();
}

bool SessionWriter::close()
{
    if (! file.is_open())
    {
        closeSyncDescriptor();
        return false;
    }

    // Taken before close(), because closing clears the stream state that says
    // whether the final header actually landed. The pad goes first so the
    // RIFF size, taken from the file's end, counts it.
    const bool padded = appendPadByteIfOdd();
    const bool headerLanded = rewriteHeaderSizes() && padded;

    file.close();

    if (headerLanded)
    {
        closeSyncDescriptor();
        // file.good() is false after a successful close on some
        // implementations, so the close itself is judged by fail(), not good().
        return ! file.fail();
    }

    // The take's own stream could not be patched, and on a full card that is
    // the normal outcome rather than a rare one: seeking to the header first
    // flushes whatever audio is still buffered, that flush has nowhere to go,
    // and the seek fails with it -- so the four bytes that say how long the
    // recording is were never written, however many times it was tried.
    //
    // The audio itself is on the card and perfectly good. Only the header is
    // wrong, and it is wrong in the way that matters most: RIFF size 0 and
    // data size 0 mean every player and DAW opens the file and sees an empty
    // recording. Measured on a real full filesystem, two of three stems held
    // about 2.4 seconds each and both declared themselves empty.
    //
    // A fresh handle has no failed state and nothing pending, so it can do
    // what this stream no longer can. Done after close() so the size on disk
    // is final.
    const bool patched = patchHeaderThroughFreshHandle();
    closeSyncDescriptor();
    return patched;
}

bool SessionWriter::patchHeaderThroughFreshHandle()
{
    uint64_t onDisk = 0;

#if ! defined (_WIN32)
    // Through the held descriptor when there is one, for the same reason the
    // sync uses it: the take's folder may have been renamed since it began.
    if (syncDescriptor >= 0)
    {
        struct stat info {};
        if (::fstat (syncDescriptor, &info) != 0)
            return false;

        onDisk = static_cast<uint64_t> (info.st_size);
    }
    else
#endif
    {
        std::error_code ec;
        onDisk = static_cast<uint64_t> (std::filesystem::file_size (pathFromUtf8 (currentFilePath), ec));

        if (ec)
            return false;
    }

    // Where the audio starts: the data size field, then the four bytes of the
    // field itself.
    const auto dataStart = static_cast<uint64_t> (dataSizeFieldPos) + 4;

    if (onDisk < dataStart)
        return false;

    // Measured from the file, never from dataBytesWrittenToCurrentFile. That
    // counter says what the writer TRIED to send, and on a full card the last
    // of it never landed -- a header built from it would overstate the audio
    // and send a reader off the end of the file, which is a worse failure than
    // the one being fixed.
    //
    // Rounded down to a whole frame, because the drive gave out mid-frame: the
    // raw figure was one byte past a frame boundary on a 24-bit mono stem, and
    // a data chunk that is not a multiple of the block alignment is malformed.
    // The stray bytes are left outside the chunk rather than described.
    const auto blockAlign = static_cast<uint64_t> (std::max (1, bytesPerSample()))
                          * static_cast<uint64_t> (std::max (1, numChannels));

    const auto wholeFrameBytes = ((onDisk - dataStart) / blockAlign) * blockAlign;

    if (wholeFrameBytes == 0)
        return false;

    const auto dataSize = static_cast<uint32_t> (
        std::min<uint64_t> (wholeFrameBytes, 0xFFFFFFFFull));

    // The file ends where the last whole frame ends, so RIFF describes exactly
    // what the data chunk describes and nothing dangles past it.
    const auto describedEnd = dataStart + wholeFrameBytes;
    const auto riffSize = static_cast<uint32_t> (
        std::min<uint64_t> (describedEnd >= 8 ? describedEnd - 8 : 0, 0xFFFFFFFFull));

#if ! defined (_WIN32)
    if (syncDescriptor >= 0)
    {
        const auto putU32 = [this] (std::streampos at, uint32_t v)
        {
            const unsigned char b[4] = { static_cast<unsigned char> (v & 0xFF),
                                         static_cast<unsigned char> ((v >> 8) & 0xFF),
                                         static_cast<unsigned char> ((v >> 16) & 0xFF),
                                         static_cast<unsigned char> ((v >> 24) & 0xFF) };
            return ::pwrite (syncDescriptor, b, 4, static_cast<off_t> (at)) == 4;
        };

        return putU32 (riffSizeFieldPos, riffSize) && putU32 (dataSizeFieldPos, dataSize);
    }
#endif

    std::fstream patch (pathFromUtf8 (currentFilePath), std::ios::in | std::ios::out | std::ios::binary);

    if (! patch.is_open())
        return false;

    patch.seekp (riffSizeFieldPos);
    writeU32LE (patch, riffSize);
    patch.seekp (dataSizeFieldPos);
    writeU32LE (patch, dataSize);
    patch.flush();

    return patch.good();
}

} // namespace mma
