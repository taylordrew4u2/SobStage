#include "SessionRecovery.h"
#include "TakeCompleteness.h"
#include "Utf8Path.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace mma {

namespace {

uint32_t readU32LE (const std::array<char, 4>& b)
{
    return static_cast<uint32_t> (static_cast<unsigned char> (b[0]))
         | (static_cast<uint32_t> (static_cast<unsigned char> (b[1])) << 8)
         | (static_cast<uint32_t> (static_cast<unsigned char> (b[2])) << 16)
         | (static_cast<uint32_t> (static_cast<unsigned char> (b[3])) << 24);
}

bool readTag (std::fstream& f, std::array<char, 4>& out)
{
    f.read (out.data(), 4);
    return f.gcount() == 4;
}

bool readU32 (std::fstream& f, uint32_t& out)
{
    std::array<char, 4> b {};
    if (! readTag (f, b))
        return false;

    out = readU32LE (b);
    return true;
}

void writeU32LE (std::fstream& f, std::streampos at, uint32_t value)
{
    const char bytes[4] = {
        static_cast<char> (value & 0xFF),
        static_cast<char> ((value >> 8) & 0xFF),
        static_cast<char> ((value >> 16) & 0xFF),
        static_cast<char> ((value >> 24) & 0xFF)
    };

    f.seekp (at);
    f.write (bytes, 4);
}

bool tagIs (const std::array<char, 4>& tag, const char* expected)
{
    return std::equal (tag.begin(), tag.end(), expected);
}

} // namespace

bool SessionRecovery::sessionWasInterrupted (const SessionMetadata& meta)
{
    return meta.stopTimestampIso.empty();
}

std::optional<std::string> SessionRecovery::readExactly (std::istream& in, uint64_t expectedBytes)
{
    // A details file is a few kilobytes. Anything claiming to be vastly more
    // is not one this app wrote, and is not worth a giant allocation on a
    // detached worker to find that out.
    constexpr uint64_t kLargestRecordBytes = 64ull * 1024ull * 1024ull;

    if (expectedBytes > kLargestRecordBytes)
        return std::nullopt;

    std::string bytes (static_cast<size_t> (expectedBytes), '\0');

    if (expectedBytes > 0)
        in.read (bytes.data(), static_cast<std::streamsize> (expectedBytes));

    if (in.bad() || static_cast<uint64_t> (in.gcount()) != expectedBytes)
        return std::nullopt;

    return bytes;
}

std::optional<std::string> SessionRecovery::readWholeFileChecked (const std::string& path)
{
    try
    {
        std::ifstream in (pathFromUtf8 (path), std::ios::in | std::ios::binary);

        if (! in.is_open())
            return std::nullopt;

        // The size of the file this stream has open, not of whatever the path
        // names a moment later: session.json is replaced by a rename.
        in.seekg (0, std::ios::end);
        const auto end = in.tellg();

        if (! in || end < 0)
            return std::nullopt;

        in.seekg (0, std::ios::beg);

        if (! in)
            return std::nullopt;

        return readExactly (in, static_cast<uint64_t> (end));
    }
    catch (...)
    {
        return std::nullopt;
    }
}

int RecoveredSession::keptFileCount() const
{
    return static_cast<int> (std::count_if (files.begin(), files.end(),
                                            [] (const RecoveredFile& f) { return ! f.reportedEmpty; }));
}

int RecoveredSession::playableFileCount() const
{
    return static_cast<int> (std::count_if (files.begin(), files.end(),
                                            [] (const RecoveredFile& f)
                                            { return ! f.reportedEmpty && ! f.repairFailed; }));
}

int RecoveredSession::emptyFileCount() const
{
    return static_cast<int> (std::count_if (files.begin(), files.end(),
                                            [] (const RecoveredFile& f) { return f.reportedEmpty; }));
}

double RecoveredSession::longestSeconds() const
{
    double longest = 0.0;

    for (const auto& f : files)
        if (! f.reportedEmpty)
            longest = std::max (longest, f.seconds);

    return longest;
}

RecoveredFile SessionRecovery::repairWavFile (const std::string& path)
{
    RecoveredFile result;

    // The name only, so the caller can show it without a path down the side of
    // a panel. The separator check covers both platforms' folders.
    const auto slash = path.find_last_of ("/\\");
    result.fileName = slash == std::string::npos ? path : path.substr (slash + 1);
    result.reportedEmpty = true; // until proven otherwise

    // UTF-8 on the way in; see Utf8Path.h for what a narrow open does on Windows.
    const auto nativePath = pathFromUtf8 (path);
    std::fstream file (nativePath, std::ios::in | std::ios::out | std::ios::binary);

    if (! file.is_open())
    {
        // Two different things arrive here and they need different answers.
        //
        // A path with nothing at it is nothing: no audio was lost, and calling
        // it empty is exactly right. A file that IS there and will not open for
        // writing is a recording of unknown length on a card that has gone
        // read-only -- calling that one empty sends the user away from audio
        // that may be perfectly intact.
        // std::filesystem::exists, not an ifstream open. Opening was a proxy
        // for "is there something here", and it answers differently for a
        // directory on Windows than on Linux -- so the question is asked
        // directly. An error_code overload because a path that cannot even be
        // interrogated is, for our purposes, a path with nothing at it.
        std::error_code ec;

        if (! std::filesystem::exists (nativePath, ec) || ec)
            return result;

        // A file this app cannot even open is not a file that holds under a
        // second of audio, and reporting it as one -- which is what
        // reportedEmpty alone said -- sends the user away from a recording that
        // may be perfectly intact on a card that has gone read-only.
        //
        // reportedEmpty is cleared as well as repairFailed set, or the panel
        // goes on counting it under "empty file left alone" and a folder of
        // nothing but unopenable files still fails isWorthPresenting() and is
        // announced as one where nothing survived. Nobody knows whether
        // anything survived -- that is the whole point -- so it is listed, and
        // the per-file warning says it could not be read.
        result.reportedEmpty = false;
        result.repairFailed = true;
        return result;
    }

    file.seekg (0, std::ios::end);
    const auto fileSize = static_cast<uint64_t> (file.tellg());
    file.seekg (0, std::ios::beg);

    std::array<char, 4> tag {};
    uint32_t riffSize = 0;
    std::array<char, 4> waveTag {};

    if (! readTag (file, tag) || ! tagIs (tag, "RIFF")
        || ! readU32 (file, riffSize)
        || ! readTag (file, waveTag) || ! tagIs (waveTag, "WAVE"))
        return result;

    const std::streampos riffSizeFieldPos = 4;

    // Walk the chunks rather than assuming where data starts. The writer puts a
    // bext chunk between fmt and data (§6.1), and assuming a fixed offset would
    // break the moment that chunk changed size.
    uint32_t channels = 0, sampleRate = 0, bitsPerSample = 0;
    std::streampos dataSizeFieldPos = 0;
    uint64_t dataStart = 0;
    uint32_t declaredDataSize = 0;
    bool foundData = false;

    while (file && static_cast<uint64_t> (file.tellg()) + 8 <= fileSize)
    {
        std::array<char, 4> chunkTag {};
        uint32_t chunkSize = 0;

        if (! readTag (file, chunkTag) || ! readU32 (file, chunkSize))
            break;

        const auto chunkStart = static_cast<uint64_t> (file.tellg());
        const uint64_t paddedSize = static_cast<uint64_t> (chunkSize) + (chunkSize & 1u);
        // The data size may be stale after a crash; other chunks must fit on
        // disk. Widen before adding and seek absolutely so corrupt sizes can
        // neither wrap nor send the parser backwards.
        if (! tagIs (chunkTag, "data") && paddedSize > fileSize - chunkStart)
            return result;

        if (tagIs (chunkTag, "fmt "))
        {
            if (chunkSize < 16)
                return result;

            std::array<char, 4> field {};
            file.read (field.data(), 2); // audio format, unused
            file.read (field.data(), 2);
            channels = static_cast<uint32_t> (static_cast<unsigned char> (field[0]))
                     | (static_cast<uint32_t> (static_cast<unsigned char> (field[1])) << 8);
            readU32 (file, sampleRate);
            uint32_t byteRate = 0;
            readU32 (file, byteRate);
            file.read (field.data(), 2); // block align, recomputed below
            file.read (field.data(), 2);
            bitsPerSample = static_cast<uint32_t> (static_cast<unsigned char> (field[0]))
                          | (static_cast<uint32_t> (static_cast<unsigned char> (field[1])) << 8);

            // Skip any remainder of an extended fmt chunk.
            file.seekg (static_cast<std::streamoff> (chunkStart + paddedSize), std::ios::beg);
        }
        else if (tagIs (chunkTag, "data"))
        {
            dataSizeFieldPos = static_cast<std::streamoff> (file.tellg()) - 4;
            dataStart = static_cast<uint64_t> (file.tellg());
            declaredDataSize = chunkSize;
            foundData = true;
            break;
        }
        else
        {
            // Chunks are word-aligned, so an odd size carries a pad byte.
            file.seekg (static_cast<std::streamoff> (chunkStart + paddedSize), std::ios::beg);
        }
    }

    if (! foundData || channels == 0 || sampleRate == 0 || bitsPerSample == 0)
        return result;

    const uint32_t blockAlign = channels * (bitsPerSample / 8);

    if (blockAlign == 0 || fileSize < dataStart)
        return result;

    // What is actually there, as opposed to what the header last admitted to.
    const uint64_t actualDataBytes = fileSize - dataStart;
    const uint64_t wholeFrameBytes = (actualDataBytes / blockAlign) * blockAlign;

    result.frames = wholeFrameBytes / blockAlign;
    result.seconds = static_cast<double> (result.frames) / static_cast<double> (sampleRate);
    // Chunks are word-aligned: an odd data chunk (mono 24-bit, odd frames) is
    // followed by a pad byte the RIFF size counts, as SessionWriter's
    // appendPadByteIfOdd does on a clean stop. Without it the repaired file's
    // RIFF size stops one byte short of the chunk's padded end.
    const uint64_t padBytes = wholeFrameBytes & 1u;
    // RIFF size counts everything after the size field itself.
    const uint64_t expectedRiffSize = dataStart + wholeFrameBytes + padBytes - 8;

    result.headerWasStale = declaredDataSize != wholeFrameBytes || riffSize != expectedRiffSize;

    if (result.headerWasStale)
    {
        writeU32LE (file, dataSizeFieldPos, static_cast<uint32_t> (wholeFrameBytes));
        writeU32LE (file, riffSizeFieldPos, static_cast<uint32_t> (expectedRiffSize));

        // Overwrites a trailing partial-frame byte if one is there, which is
        // not audio anyone can play; appends one otherwise.
        if (padBytes != 0)
        {
            file.seekp (static_cast<std::streamoff> (dataStart + wholeFrameBytes), std::ios::beg);
            file.put ('\0');
        }

        file.flush();

        // Checked. A card that is read-only, full or failing takes the repair
        // and drops it, and this used to report the file as repaired anyway --
        // the user then meets the same broken header in whatever they open it
        // with, having been told it was fixed.
        result.repairFailed = ! file.good();
    }

    // §6.6: under a second is a stub. Reported as empty rather than offered --
    // and left on disk rather than deleted, because silently removing something
    // off a user's card at launch is a worse mistake than listing a short file.
    result.reportedEmpty = result.seconds < kMinimumUsefulSeconds;

    return result;
}

namespace {

std::string folderName (const std::string& folder)
{
    auto end = folder.find_last_not_of ("/\\");
    if (end == std::string::npos)
        return {};

    const auto start = folder.find_last_of ("/\\", end);
    return folder.substr (start == std::string::npos ? 0 : start + 1,
                          end - (start == std::string::npos ? 0 : start + 1) + 1);
}

} // namespace

namespace {

std::string describeLength (double seconds)
{
    const auto total = static_cast<long> (std::lround (seconds));
    const auto minutes = total / 60;
    const auto remainder = total % 60;

    return minutes > 0 ? std::to_string (minutes) + "m " + std::to_string (remainder) + "s"
                       : std::to_string (remainder) + "s";
}

} // namespace

RecoveredTakeRow recoveredTakeRow (const RecoveredSession& session)
{
    RecoveredTakeRow row;
    row.folderName = folderName (session.folder);
    row.fullPath = session.folder;
    row.fileCount = session.keptFileCount();
    row.playableFileCount = session.playableFileCount();
    row.emptyFileCount = session.emptyFileCount();
    row.longestSeconds = session.longestSeconds();
    row.movieCount = session.movieCount;
    row.quickTimeMovieCount = session.quickTimeMovieCount;
    row.isBackupBecauseCardCopyUnrepairable = session.shownBecauseCardCopyUnrepairable;
    row.moviesInCardCopy = session.moviesInCardCopy;
    return row;
}

std::string recoveredTakesExplanation (const std::vector<RecoveredTakeRow>& takes)
{
    // A file the card would not take the repair on still has its old header,
    // so "playable" is only said when it is true of every file listed.
    const bool allPlayable = std::all_of (takes.begin(), takes.end(),
                                          [] (const RecoveredTakeRow& t)
                                          { return t.playableFileCount == t.fileCount; });

    const std::string opening = takes.size() == 1
        ? "The app stopped before this take was finished -- a crash, a power cut, "
          "or the card coming out. The sound was still on the disk"
        : "The app stopped before these takes were finished. The sound was still on the disk";

    if (allPlayable)
        return opening + ", and it has been repaired and is playable.";

    return opening + ", but some files couldn't be repaired on this card. "
                     "Copy them off the card before playing them.";
}

std::string recoveredTakeDetail (const RecoveredTakeRow& take)
{
    std::string detail = std::to_string (take.fileCount)
                       + (take.fileCount == 1 ? " file, " : " files, ")
                       + describeLength (take.longestSeconds) + " of sound";

    // Said per take, so the user knows which folder the warning above is about.
    const int unrepaired = take.fileCount - take.playableFileCount;

    if (unrepaired > 0)
        detail += ", " + std::to_string (unrepaired) + " couldn't be repaired";

    // Named rather than hidden: §6.6 would rather report a stub as empty
    // than present it, and a user counting files needs to know why there
    // are fewer than they expected.
    if (take.emptyFileCount > 0)
        detail += ", and " + std::to_string (take.emptyFileCount)
                + (take.emptyFileCount == 1 ? " empty file left alone"
                                            : " empty files left alone");

    // Said, or the user opens a folder in the backup and wonders why it is
    // not the card they recorded to.
    if (take.isBackupBecauseCardCopyUnrepairable)
        detail += "; the card's copy couldn't be repaired, so this is the local backup copy";

    // The card said only what happened to the sound, and the camera movies in
    // the same folder were not mentioned at all -- so the first anyone heard
    // of a movie cut short was opening it. Nothing here repairs a movie.
    if (take.movieCount > 0)
    {
        detail += "; " + std::to_string (take.movieCount)
                + (take.movieCount == 1 ? " camera movie" : " camera movies");

        // Movies are never backed up. A backup copy offered in the card's
        // place has none beside it, and the folder Open goes to would leave
        // them unaccounted for.
        if (take.moviesInCardCopy)
            detail += " on the card only";

        // AVFoundation writes a QuickTime movie one ten-second fragment at a
        // time, so an interrupted one plays up to its last fragment, and one
        // shorter than that may not open at all. That is promised of .mov
        // only: what a crash leaves of another container -- the .wmv a
        // Windows camera writes -- is not known here, so it is not guessed at.
        if (take.quickTimeMovieCount == take.movieCount)
            detail += take.longestSeconds < 10.0 ? ", which may not open"
                                                 : ", which may end up to 10 s early";
        else
            detail += take.movieCount == 1 ? ", which wasn't finished and may not open"
                                           : ", which weren't finished and may not open";
    }

    return detail;
}

void SessionRecovery::countCameraMovie (RecoveredSession& session, const std::string& fileName,
                                        const std::string& cameraExtension)
{
    if (isSystemClutterFile (fileName))
        return;

    const auto endsWith = [&fileName] (const std::string& extension)
    {
        if (extension.empty() || fileName.size() <= extension.size())
            return false;

        return std::equal (extension.rbegin(), extension.rend(), fileName.rbegin(),
                           [] (char a, char b)
                           {
                               return std::tolower (static_cast<unsigned char> (a))
                                   == std::tolower (static_cast<unsigned char> (b));
                           });
    };

    if (! (endsWith (cameraExtension) || endsWith (".mov") || endsWith (".mp4") || endsWith (".wmv")))
        return;

    ++session.movieCount;

    if (endsWith (".mov"))
        ++session.quickTimeMovieCount;
}

bool SessionRecovery::backupCanStandInForCard (const RecoveredSession& card, const RecoveredSession& backup)
{
    // Only a card copy in trouble is ever passed over.
    if (card.playableFileCount() >= card.keptFileCount())
        return false;

    // A backup that stopped mid-take ends early, whatever its headers say.
    // Either copy's record can know: the card's session.json may have been
    // refreshed after the backup's drive stopped taking writes.
    if (card.backupCopyCutShort || backup.backupCopyCutShort)
        return false;

    // Every file the card copy has, and every one of them playable. A file the
    // card would not even open counts as one the card has -- nobody knows
    // what is in it, which is the point.
    if (backup.playableFileCount() != backup.keptFileCount()
        || backup.keptFileCount() < card.keptFileCount())
        return false;

    // Not shorter than what could be measured on the card. A file the card
    // would not open has no length here, so a locked card's copy measures
    // nothing; one that opened but refused the repair still has its bytes
    // counted, and a backup missing the end of them is not the better copy.
    return backup.longestSeconds() + kBackupLengthToleranceSeconds >= card.longestSeconds();
}

RecoveredSessionList SessionRecovery::mergeScan (RecoveredSessionList list,
                                                 std::vector<RecoveredSession> scanned,
                                                 bool scanIsPrimaryCopy)
{
    for (auto& session : scanned)
    {
        const auto name = folderName (session.folder);
        const auto existing = std::find_if (list.shown.begin(), list.shown.end(),
                                            [&] (const RecoveredSession& candidate)
                                            {
                                                if (folderName (candidate.folder) == name)
                                                    return true;

                                                // The card copy names its backup. A backup
                                                // folder that had to take a "_2" was shown as
                                                // a second interrupted take beside the first.
                                                const auto& primary = scanIsPrimaryCopy ? session : candidate;
                                                const auto& backup = scanIsPrimaryCopy ? candidate : session;
                                                return ! primary.mirrorFolder.empty()
                                                    && primary.mirrorFolder == backup.folder;
                                            });

        // The card's copy normally wins. Not when the card refused the repair
        // (locked, read-only, failing) and the backup took it: the card copy
        // then still has its broken header, and the button sent the user to
        // it while a playable copy of the same take sat in the backup folder.
        // The backup stands in for it, carrying the card copy's movie count --
        // movies are never backed up -- so the row still accounts for them.
        const auto standIn = [] (RecoveredSession& backup, const RecoveredSession& card)
        {
            backup.shownBecauseCardCopyUnrepairable = true;
            backup.moviesInCardCopy = card.movieCount > 0;
            backup.movieCount = card.movieCount;
            backup.quickTimeMovieCount = card.quickTimeMovieCount;
        };

        if (existing == list.shown.end())
        {
            list.shown.push_back (std::move (session));
        }
        else if (scanIsPrimaryCopy && backupCanStandInForCard (session, *existing))
        {
            standIn (*existing, session);
            list.hiddenFolders.push_back (std::move (session.folder));
        }
        else if (! scanIsPrimaryCopy && backupCanStandInForCard (*existing, session))
        {
            standIn (session, *existing);
            list.hiddenFolders.push_back (std::move (existing->folder));
            *existing = std::move (session);
        }
        else if (scanIsPrimaryCopy)
        {
            // The user's primary copy wins; the backup it displaces is hidden,
            // not forgotten.
            list.hiddenFolders.push_back (std::move (existing->folder));
            *existing = std::move (session);
        }
        else
        {
            list.hiddenFolders.push_back (std::move (session.folder));
        }
    }

    // Newest first. The card's button is "Open the newest" and opens the first
    // entry, which was simply whichever root's scan was merged first -- the
    // backup folder's take from last week over the one interrupted today.
    std::stable_sort (list.shown.begin(), list.shown.end(),
                      [] (const RecoveredSession& a, const RecoveredSession& b)
                      {
                          return a.modifiedMs > b.modifiedMs;
                      });

    return list;
}

} // namespace mma
