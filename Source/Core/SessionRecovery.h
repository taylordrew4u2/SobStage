#pragma once
#include "SessionMetadata.h"
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace mma {

/// One file found in an interrupted session folder, after its header has been
/// made to agree with the bytes actually on disk.
struct RecoveredFile
{
    std::string fileName;
    uint64_t frames = 0;
    double seconds = 0.0;
    /// True when the sizes in the header did not match the file, which is what
    /// a take killed between two header rewrites looks like.
    bool headerWasStale = false;
    /// §6.6: under a second of audio is an unplayable stub, not a recording.
    bool reportedEmpty = false;
    /// True when the header was stale AND the repair could not be written back.
    ///
    /// The repair result was never checked, so a file on a read-only or failing
    /// card was reported as repaired and handed to the user with the same wrong
    /// header it started with -- and they would only find out on opening it in
    /// something else.
    bool repairFailed = false;
};

/// An interrupted take: one whose session.json never got a stop timestamp.
struct RecoveredSession
{
    std::string folder;
    std::string startedIso;
    std::vector<RecoveredFile> files;
    /// Where this take's session.json said its local backup copy was going.
    /// The backup folder is not always named like the card's -- one that
    /// already existed in the backup root gets a "_2" -- so this, not the name
    /// alone, is what ties the two copies of one take together.
    std::string mirrorFolder;
    /// The folder's modification time, milliseconds since 1970. Orders the
    /// card newest first across both roots: the scans finish in either order.
    int64_t modifiedMs = 0;
    /// Camera movies beside the audio, in whatever container the camera
    /// wrote (see countCameraMovie). Nothing repairs them.
    int movieCount = 0;
    /// Of those, the QuickTime movies (.mov) AVFoundation writes on a Mac: it
    /// writes a movie fragment every ten seconds, so one cut off by a crash
    /// opens up to its last fragment -- or not at all if the take was
    /// shorter. A Windows camera's .wmv is not written that way, and nothing
    /// is promised about where it ends.
    int quickTimeMovieCount = 0;
    /// The take's own record says its local backup copy had stopped being
    /// written before the record was last refreshed -- the backup's drive
    /// filled or failed, or the backup was switched off mid-take. That copy
    /// ends early whatever its headers say, so it never stands in for the
    /// card's. Read from session.json (backup on, no longer active).
    bool backupCopyCutShort = false;
    /// This is the local backup copy, shown because the card's copy of the
    /// same take could not be repaired (a locked or read-only card) while this
    /// one could -- so "Open" goes to audio that plays.
    bool shownBecauseCardCopyUnrepairable = false;
    /// With the above: the camera movies are in the card's copy, not this one.
    /// Movies are never backed up, so the count is the card copy's, and the
    /// card says where they are rather than leaving them out.
    bool moviesInCardCopy = false;

    /// Files worth putting in front of the user: those with real audio, plus
    /// those whose length could not be established because the file would not
    /// open. The second kind is not known to hold audio -- that is the point --
    /// and calling it empty sends someone away from a recording that may be
    /// perfectly intact on a card that has gone read-only.
    int keptFileCount() const;

    /// Files that were repaired AND can actually be opened.
    ///
    /// keptFileCount() counts everything not reported empty, and a file whose
    /// repair failed deliberately sets reportedEmpty = false -- so it counts
    /// there too. The recovery headline said "its 8 files have been repaired
    /// and can be played" over a folder holding one good stem, six stubs and a
    /// file the card would not open, contradicting the per-file warning
    /// printed directly above it.
    int playableFileCount() const;
    /// Files that were there but held less than a second.
    int emptyFileCount() const;
    double longestSeconds() const;
    bool isWorthPresenting() const { return keptFileCount() > 0; }
};

/// What the recovery card shows, plus the copies it deliberately does not.
struct RecoveredSessionList
{
    std::vector<RecoveredSession> shown;
    /// Folders of same-named copies hidden behind a shown one -- the local
    /// backup of a take whose card copy is on the card. Nobody is offered
    /// them, but they are just as interrupted, so dismissing the card has to
    /// stamp them too or they come back at the next launch.
    std::vector<std::string> hiddenFolders;
};

/// One take as the recovery card lists it. Built here rather than in the panel
/// so the card's wording can be checked without a window.
struct RecoveredTakeRow
{
    std::string folderName;
    std::string fullPath;
    int fileCount = 0;
    /// Files repaired AND openable. The card used to say every take "has been
    /// repaired and is playable" over files the card had refused the repair on.
    int playableFileCount = 0;
    int emptyFileCount = 0;
    double longestSeconds = 0.0;
    int movieCount = 0;
    int quickTimeMovieCount = 0;
    /// The row is the local backup copy, because the card's copy could not be
    /// repaired; said in the detail line, or the user opens a folder that is
    /// not on the card they recorded to and wonders why.
    bool isBackupBecauseCardCopyUnrepairable = false;
    /// The movies counted above are in the card's copy, not the folder Open
    /// goes to.
    bool moviesInCardCopy = false;
};

RecoveredTakeRow recoveredTakeRow (const RecoveredSession& session);
/// The paragraph above the list: what happened, and whether it is safe to play.
std::string recoveredTakesExplanation (const std::vector<RecoveredTakeRow>& takes);
/// The line under a take's name: "2 files, 3m 4s of sound, ...".
std::string recoveredTakeDetail (const RecoveredTakeRow& take);

/// §6.6 crash and power-loss recovery.
///
/// SessionWriter already rewrites each file's RIFF and data sizes every five
/// seconds precisely so that an interrupted file stays playable. Nothing ever
/// went looking for those files afterwards, so the guarantee was written and
/// never collected: after a force-quit or a power cut the audio was on the card
/// with a header describing a file up to five seconds shorter than it really
/// was, and the app came up as though nothing had happened.
///
/// The filesystem walk lives in the App layer. What is here is the part worth
/// being sure about, and it runs on a machine with no audio hardware.
class SessionRecovery
{
public:
    /// §6.6: "discard any recovered file containing under 1 second of audio;
    /// report it as empty rather than presenting an unplayable stub."
    static constexpr double kMinimumUsefulSeconds = 1.0;

    /// A take that stopped cleanly wrote a stop timestamp. One that did not is
    /// a take the app never got to finish.
    static bool sessionWasInterrupted (const SessionMetadata& meta);

    /// Rewrites the RIFF and data chunk sizes from the bytes actually present,
    /// and reports what the file turned out to hold. Safe to run on a file
    /// whose header is already correct: it reports headerWasStale = false and
    /// writes nothing.
    ///
    /// Returns frames == 0 for anything that is not a readable WAV, rather than
    /// throwing -- a folder recovered off a card can contain anything.
    static RecoveredFile repairWavFile (const std::string& path);

    /// A take's session.json, byte for byte, or nothing when its bytes could
    /// not be read just now -- which is not the same as a file that was read
    /// and turned out empty or cut short.
    ///
    /// The two used to look identical. juce::File::loadFileAsString answers a
    /// failed open or a failed read with an empty string, the lenient parser
    /// turns that into a default record without throwing, and the record then
    /// had no stop time -- so a perfectly good take whose details file was an
    /// iCloud "Optimize Mac Storage" placeholder, opened offline, was called
    /// interrupted, and dismissing it renamed a near-empty record over the
    /// real one (a rename needs none of the old file's bytes). A file cut short
    /// by a power cut still reads, as the bytes it really holds, and is still
    /// treated as interrupted and stamped, or it would be offered at every
    /// launch.
    ///
    /// Not there, not openable, a read that fails, or fewer bytes than the file
    /// says it holds -- what a cloud-only placeholder delivers offline -- all
    /// count as "could not read". `path` is UTF-8.
    static std::optional<std::string> readWholeFileChecked (const std::string& path);

    /// The check behind readWholeFileChecked, for any stream: exactly
    /// `expectedBytes` from `in`, or nothing when it delivered fewer or failed.
    static std::optional<std::string> readExactly (std::istream& in, uint64_t expectedBytes);

    /// Counts `fileName`, one file in an interrupted take's folder, into
    /// `session` when it is a camera movie: in the container this computer's
    /// cameras write (`cameraExtension`, juce::CameraDevice's own -- ".mov"
    /// on a Mac, ".wmv" on Windows) or any other a camera writes (.mov, .mp4,
    /// .wmv), for a card recorded on another computer. Matched without regard
    /// to case. A hidden file is the operating system's (isSystemClutterFile),
    /// never a movie. Only .mov counts as a QuickTime movie.
    ///
    /// Windows' .wmv was not counted at all, so the recovery card never
    /// mentioned a Windows take's movies.
    static void countCameraMovie (RecoveredSession& session, const std::string& fileName,
                                  const std::string& cameraExtension);

    /// Adds one root's scan to the list. A take found in both the save
    /// location and the local backup -- under the same folder name, or as the
    /// backup folder the card copy's session.json names -- is shown once: the
    /// save location's copy wins, and the other copy's folder is remembered in
    /// hiddenFolders rather than forgotten. The shown list is kept newest
    /// first, because the card's button opens the first one as "the newest".
    ///
    /// The one exception: a card copy with files the card would not let be
    /// repaired (a locked, read-only or failing card), beside a backup copy
    /// that holds the whole take and repaired cleanly. Then the backup is
    /// shown, marked shownBecauseCardCopyUnrepairable, and the card copy is
    /// the hidden one -- see backupCanStandInForCard().
    static RecoveredSessionList mergeScan (RecoveredSessionList list,
                                           std::vector<RecoveredSession> scanned,
                                           bool scanIsPrimaryCopy);

    /// How much shorter than the card's audio, where that could be measured,
    /// the backup's may be and still count as the same take: the writer feeds
    /// both copies block by block, so after a crash they end together, give or
    /// take what the last moment left unwritten.
    static constexpr double kBackupLengthToleranceSeconds = 1.0;

    /// True when `backup` should be offered instead of `card`: the card copy
    /// has files whose repair the card refused, and the backup has every file
    /// the card copy has, all of them repaired and playable, was not cut short
    /// mid-take, and is not measurably shorter. Anything less and the card's
    /// copy stays the one offered, as it always was -- a backup missing the
    /// end of the take is not a better answer than a card copy that has it.
    static bool backupCanStandInForCard (const RecoveredSession& card, const RecoveredSession& backup);
};

} // namespace mma
