#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mma {

/// Where a take is written while it records.
///
/// SD cards and USB sticks are what fail under a live take: a cheap card that
/// stalls for a second, or a bus-powered reader on the microphones' hub, and
/// audio is lost. So a take meant for one is recorded on this computer's own
/// drive (the backup folder, ~/RECORDINGS-MIRROR) and copied to the card after
/// Stop, when nothing else is writing.
enum class TakeRoute
{
    /// The destination is on this computer's own disk, or copying is off.
    StraightToDestination,
    /// Record on this computer, copy to the card after Stop.
    ComputerThenCard,
    /// The destination is a card but this computer is too full to hold the
    /// take, so it records straight to the card as it always did.
    StraightToCardComputerFull,
};

struct TakeRouteInputs
{
    bool copyAfterTakeEnabled = true;
    /// 1 yes, 0 no, -1 unknown (a card that is not plugged in, or a disk the
    /// system cannot identify). Unknown is treated as "not this computer".
    int destinationOnThisComputer = -1;
    /// Free space on the disk the backup folder is on, when known.
    std::optional<int64_t> computerFreeBytes;
    /// Audio and cameras at the current settings.
    double bytesPerSecond = 0.0;
    /// How long the destination itself could record for at that rate, when
    /// known: what a take recorded straight to it could last.
    std::optional<double> destinationRemainingSeconds;
};

/// What this computer must be able to hold for a take recorded here: as much
/// as the card could hold, up to kLongestShowSeconds -- so a take recorded
/// here never stops for want of room where one recorded straight to the card
/// would have carried on. An hour when the card's room is not known (it is
/// out, so recording here is the only way anyway).
int64_t projectedTakeBytes (const TakeRouteInputs& inputs) noexcept;

inline constexpr double kLongestShowSeconds = 4.0 * 60.0 * 60.0;
inline constexpr double kPlanningSeconds = 60.0 * 60.0;

/// The route for the next take. Free space at or below projectedTakeBytes()
/// plus MirrorPolicy::kMinHeadroomBytes sends the take straight to the card:
/// the same 2 GB rule the backup copy has always followed (§1, §6.3).
TakeRoute decideTakeRoute (const TakeRouteInputs& inputs) noexcept;

/// How long Record waits for the first answer about where a save location
/// is before going on without it. A card that hangs before the status probe
/// has answered holds the probe inside that call; not knowing is treated as
/// "not this computer", so the take is recorded here -- which is what the
/// arm itself concludes after its own 1.5 s wait.
class PlacementWait
{
public:
    static constexpr double kGiveUpAfterMs = 2000.0;

    /// True once `destination` has gone unanswered for longer than
    /// kGiveUpAfterMs. A new destination, or an answer, starts the wait again.
    bool givenUp (const std::string& destination, bool answered, double nowMs)
    {
        if (answered || destination != waitingFor)
        {
            waitingFor = answered ? std::string() : destination;
            since = nowMs;
            return false;
        }

        return nowMs - since > kGiveUpAfterMs;
    }

private:
    std::string waitingFor;
    double since = 0.0;
};

namespace cardcopy {

/// Hidden file in the take's folder on this computer: the take still has to
/// be copied, and to where. Removed once every file is on the card and checked.
inline constexpr const char* kMarkerFileName = ".sobstage-card-copy.json";

/// A file on the card is written as ".MIX.wav.sobcopy" and only renamed to
/// "MIX.wav" once it has been read back and matched, so a file under its real
/// name on the card is always a complete one.
inline constexpr const char* kWorkingSuffix = ".sobcopy";

inline constexpr size_t kChunkBytes = 4u << 20;

/// Room left over on the card beyond the files themselves: the filesystem's
/// own bookkeeping, and a card never filled to its last byte.
inline constexpr uint64_t kCardMarginBytes = 16u << 20;

/// The largest file a FAT32 card can hold is one byte under this.
inline constexpr uint64_t kFat32FileLimitBytes = 1ull << 32;

struct Marker
{
    Marker() = default;
    Marker (std::string root, std::string folder = {}, std::string identity = {}, bool nextCard = false)
        : destinationRoot (std::move (root)), cardFolder (std::move (folder)), cardIdentity (std::move (identity)),
          takeNextCard (nextCard) {}

    /// The save location the take was meant for.
    std::string destinationRoot;
    /// The take's folder on the card, once chosen. Empty until then.
    std::string cardFolder;
    /// Which card (cardIdentityOf): the one in when the take was armed, or
    /// else the first the copy reached. Another card at the same place -- a
    /// Windows drive letter now a USB stick's, a camera card also called "NO
    /// NAME" -- is not written to. Empty when it could not be told.
    std::string cardIdentity;
    /// A file was too big for the take's card, and the user was told to use
    /// another card (or reformat this one): either is a card with another
    /// identity, and the next card the copy reaches is taken as the take's --
    /// not refused as "not the one this take is for", for ever. Kept on this
    /// computer with the rest, so a quit and a relaunch -- the usual moment a
    /// card is reformatted or swapped -- keep to it. Until a card is taken.
    bool takeNextCard = false;
};

std::string markerToJson (const Marker& marker);
/// Nothing for text that is not a marker this app wrote.
std::optional<Marker> markerFromJson (const std::string& text);

/// "." + name + ".sobcopy": a copy that has not been checked yet.
bool isWorkingFileName (const std::string& name);

/// Whether a file in the take's folder on this computer goes to the card:
/// not hidden (Finder's .DS_Store, AppleDouble twins, the marker), and not an
/// unfinished export's working file (isAbandonedTakeWorkingFile).
bool shouldCopy (const std::string& name);

/// Copy order: everything by name, then activity.log, then session.json. A
/// card folder holding session.json therefore holds everything else too, and
/// the card's recovery scan skips any folder without one.
std::vector<std::string> copyOrder (std::vector<std::string> names);

/// "<root>/<name>", with the separator this platform uses.
std::string joinPath (const std::string& folder, const std::string& name);
/// The folder holding `path`; `path` itself for a root.
std::string parentOf (const std::string& path);
/// The last part of `path`.
std::string lastPartOf (const std::string& path);

enum class FsError { None, NotFound, NoSpace, ReadOnly, NotAllowed, TooBig, Other };

struct FsResult
{
    FsError error = FsError::None;
    /// errno, or the Windows error code. For the journal, never shown.
    int osError = 0;
    bool ok() const { return error == FsError::None; }
};

struct Entry
{
    std::string name;
    uint64_t size = 0;
    bool isFolder = false;
};

/// The only way copyTake() touches a disk. The tests fake every call; the app
/// uses NativeCopyFileSystem (plain libc and Win32 calls).
class CopyFileSystem
{
public:
    struct Reader
    {
        virtual ~Reader() = default;
        /// `got` 0 with ok() is the end of the file.
        virtual FsResult read (void* buffer, size_t bytes, size_t& got) = 0;
    };

    struct Writer
    {
        virtual ~Writer() = default;
        virtual FsResult write (const void* data, size_t bytes) = 0;
        /// Flushed to the card itself (F_FULLFSYNC / fsync / FlushFileBuffers),
        /// then closed. Nothing is renamed before this succeeds.
        virtual FsResult syncAndClose() = 0;
    };

    virtual ~CopyFileSystem() = default;

    virtual bool exists (const std::string& path) = 0;
    virtual FsResult list (const std::string& folder, std::vector<Entry>& entries) = 0;
    /// One level only, never "mkdir -p": a missing card's mount point must
    /// never be made on the startup disk.
    virtual FsResult makeFolder (const std::string& path) = 0;
    virtual std::optional<uint64_t> freeBytes (const std::string& path) = 0;
    virtual std::optional<bool> limitedTo4GiBFiles (const std::string& path) = 0;
    /// The format's name as the system gives it ("msdos", "exfat", "ntfs",
    /// "FAT32"), or empty. Only asked for the wording of a refused write.
    virtual std::string filesystemTypeName (const std::string&) { return {}; }
    /// Which card is at `path` (cardIdentityOf), or empty when that cannot be
    /// told. Never stands for "no card": that is asked with exists().
    virtual std::string cardIdentity (const std::string&) { return {}; }
    virtual FsResult readText (const std::string& path, std::string& text) = 0;
    /// Written to a temporary file, flushed, and renamed over `path`.
    virtual FsResult replaceText (const std::string& path, const std::string& text) = 0;
    /// `bypassCache` reads what is on the device rather than what this
    /// computer still holds in memory from writing it.
    virtual std::unique_ptr<Reader> openRead (const std::string& path, bool bypassCache, FsResult& result) = 0;
    /// Fails if `path` exists.
    virtual std::unique_ptr<Writer> createNew (const std::string& path, FsResult& result) = 0;
    virtual FsResult rename (const std::string& from, const std::string& to, bool replace) = 0;
    virtual FsResult remove (const std::string& path) = 0;
    /// The folder's own entries flushed to the card, so a rename in it is on
    /// the card itself and not only in this computer's memory. A FAT or ExFAT
    /// card pulled without ejecting otherwise loses renames made seconds
    /// before.
    virtual FsResult syncFolder (const std::string& path) = 0;
};

enum class Outcome
{
    Copied,
    NotFinishedYet,      ///< session.json missing or has no stop time: still recording, or interrupted
    ComputerCopyMissing, ///< the take's folder on this computer is gone
    ComputerReadFailed,  ///< a file on this computer could not be opened or read, its folder still there
    CardMissing,
    /// Another card is at the save location's place, not the one the take is
    /// for (Marker::cardIdentity). Nothing was written to it.
    DifferentCard,
    CardFull,
    CardLocked,
    NotAllowed,
    FileTooBig,
    NameTakenOnCard,
    CopyDidNotMatch,
    CardWriteFailed,
    Yielded,             ///< a take started: stopped at a chunk boundary, to be run again
    Cancelled            ///< quitting
};

struct Control
{
    const std::atomic<bool>* cancel = nullptr;
    /// Polled before every chunk, written or read back.
    std::function<bool()> shouldYield;
    std::function<void (uint64_t done, uint64_t total)> progress;
    /// A combined video or podcast copy is still being made in the take's
    /// folder: a copy that finishes leaves the marker on this computer, so a
    /// quit or a crash before that file is copied too picks it up at the next
    /// launch.
    bool keepMarker = false;
    /// "cardPath|size" of every file already checked on the card in this app
    /// run, so a re-run after a combined video or podcast copy is added reads
    /// back only what is new. Trusted only while the card's listing still
    /// shows the file under its name and size, and forgotten for the whole
    /// take when an attempt fails in a way that puts the card in doubt.
    std::set<std::string>* verified = nullptr;
    /// The take's files, by size, as an earlier attempt in this run left them
    /// copied (Result::filesOnCard). With it, a failed attempt works out
    /// whether this is only a file added since (Result::addedFilesOnly).
    const std::map<std::string, uint64_t>* copiedBefore = nullptr;
    /// The user's "Copy again": the card in the reader now is the one wanted,
    /// whichever card the take was for. It becomes the take's card.
    bool anyCard = false;
    /// The take was stopped in this run of the app, its files all closed: it
    /// is finished whatever its session.json says. A stop-time record that
    /// could not be written -- this computer's disk full, the reason the take
    /// stopped -- is not a take still recording, and must not keep it from
    /// the card until the next launch.
    bool takeIsFinished = false;
};

struct Result
{
    Outcome outcome = Outcome::Cancelled;
    /// The take's folder on the card, once chosen.
    std::string cardFolder;
    /// The file the outcome is about, where there is one.
    std::string fileName;
    int osError = 0;
    /// For a refused write's wording: the card's format, or empty.
    std::string filesystemType;
    uint64_t bytesCopied = 0;
    int filesCopied = 0;
    /// An earlier attempt copied the take (Control::copiedBefore), and every
    /// file it copied is on the card this attempt found -- or the card could
    /// not be looked at, as when it is out: what failed is only a file added
    /// to the folder since (a combined video, a podcast copy). False when the
    /// card in the reader was looked at and does not hold the take: another
    /// card at the same place ("NO NAME"), or one emptied since.
    bool addedFilesOnly = false;
    /// Copied: every file of the take now on the card, by size.
    std::map<std::string, uint64_t> filesOnCard;
    /// The card the take is for now (Marker::cardIdentity), once known, for
    /// the next attempt.
    std::string cardIdentity;
    /// The take did not know which card it was for -- none had ever been seen
    /// at its save location when it was armed -- and the card this attempt
    /// reached became its card. Said with "Copied": it may be a camera card
    /// also called "NO NAME", or a USB stick given the reader's letter.
    bool cardFirstSeen = false;
};

/// Copies the finished take in `localFolder` to the card, checks every file
/// against the computer's copy, and only then gives it its real name. Never
/// changes anything in `localFolder` but the marker. Safe to run again at any
/// point: files already on the card and matching are left alone.
Result copyTake (CopyFileSystem& fs, const std::string& localFolder, const Marker& marker, Control& control);

/// Whose words a refusal is put in: a fix on a Mac (Disk Utility, System
/// Settings) means nothing on Windows, and the other way round. Linux, which
/// only runs the test gates, has the Mac's.
enum class Wording { Mac, Windows };

#if defined(_WIN32)
inline constexpr Wording kThisSystemsWording = Wording::Windows;
#else
inline constexpr Wording kThisSystemsWording = Wording::Mac;
#endif

/// The button that copies a take waiting for its own card to the card in
/// the reader now instead: under Record, for every such take, and on the
/// saved-take card for its own. Named in describe()'s words, so the words
/// point at a button that is there.
inline constexpr const char* kCopyToThisCard = "Copy to this card";

/// When the words are read. While SobStage stays open, a take waiting for
/// its card is copied once the card is back; read by someone quitting, only
/// the next time SobStage is opened with the card in. "Will be copied when
/// the card is back" beside a Quit Anyway button reads as a card that fills
/// itself in.
enum class Moment { WhileOpen, Quitting };

/// What the user is told about `result`, in one or two plain sentences.
/// Empty for Yielded and Cancelled, which are not news.
std::string describe (const Result& result, const std::string& localFolder,
                      Wording wording = kThisSystemsWording, Moment moment = Moment::WhileOpen);

/// "Copying to the card: 40%."
std::string copyingLine (uint64_t done, uint64_t total);

/// After the card has not answered for a while.
std::string stalledLine (const std::string& localFolder);

/// "Waiting to copy 2 takes to the card."
std::string waitingLine (int takes);

/// Takes held back by a check of the card that has not finished: the card
/// stopped answering before their copies could begin.
std::string notAnsweringLine (int takes, const std::string& computerFolder);

/// Said when a take goes straight to the card because this computer is too
/// full. Points only at takes already on the card: some in `computerFolder`
/// may be the only copy of a take not copied yet, and those are named
/// (`takesStillToCopy`, their folders' names), because nothing a user sees in
/// Finder or File Explorer tells them from the takes that are on the card.
std::string computerFullNote (const std::string& computerFolder, const std::vector<std::string>& takesStillToCopy);

/// Whether an outcome is one of the copy's failures, reported to the user.
bool isFailure (Outcome outcome) noexcept;

/// Asked before quitting when copies to the card failed after the quit was
/// asked for -- the one "Quit When Done" was waiting for, typically. Each
/// take's last result and its folder on this computer; says where each is
/// safe. Empty for none.
std::string copyFailedBeforeQuitting (const std::vector<std::pair<Result, std::string>>& failures,
                                      Wording wording = kThisSystemsWording);

/// The copies to the card that failed after a quit was asked for, kept to be
/// asked about before the app goes (copyFailedBeforeQuitting): the copy
/// "Quit When Done" waited for, typically.
///
/// Every failure while watching is kept, a repeat of the take's last one
/// included. The activity log leaves a repeat out -- a retry that found the
/// card still out is not news there -- but a quit that waited for that retry
/// and then went without a word left the user with a card that did not hold
/// the take. And a take copied after all is no longer a failure to ask about.
class FailuresWhileQuitting
{
public:
    /// The quit was asked for. Asked again while already watching -- the quit
    /// asks on every poll while the cameras finish -- it changes nothing.
    void startWatching();

    /// The quit is off: "Don't Quit", or a take started while it waited.
    /// Nothing kept is asked about at some later quit.
    void stopWatching();

    bool isWatching() const noexcept { return watching; }

    /// A finished attempt for the take started at `localFolder`, now at
    /// `folderNow`. A failure is kept while watching, as that take's latest;
    /// one that copied the take drops whatever was kept for it.
    void noteFinished (const std::string& localFolder, const std::string& folderNow, const Result& result);

    /// Each take's latest failure not yet told, with where it is now, in the
    /// order they failed.
    std::vector<std::pair<Result, std::string>> untold() const;

    /// Everything kept so far has been asked about.
    void told() noexcept;

private:
    struct Kept
    {
        std::string localFolder, folderNow;
        Result result;
        bool told = false;
    };

    std::vector<Kept> kept;
    bool watching = false;
};

/// The takes whose copy did not finish, as the line under Record counts them.
struct Tally
{
    /// Not on the card at all: only on this computer.
    int onlyOnComputer = 0;
    /// How many of those because the card was not plugged in.
    int cardMissing = 0;
    /// How many of those because another card is in the take's card's place.
    int differentCard = 0;
    /// On the card but for a file too big for the card's format (FAT32's
    /// 4 GB), which stays on this computer. Not counted above.
    int partlyOnCard = 0;
    /// On the card but for a file added since that has not gone yet. Not
    /// counted above.
    int addedFilesWaiting = 0;
};

/// The line under Record once nothing is copying or waiting; empty when every
/// take is on the card. Never says a take is only on this computer when the
/// rest of it is on the card, which its saved-take card says.
std::string tallyLine (const Tally& tally);

/// Which card a take armed now is for: the one seen at its save location
/// (`seenNow`), or, with the card out or not looked at yet, the last one seen
/// there (`lastSeen`, by save location). Empty when no card was ever seen
/// there: then the first card the copy reaches becomes the take's, and is
/// said to be (Result::cardFirstSeen).
std::string cardForTake (const std::string& seenNow, const std::map<std::string, std::string>& lastSeen,
                         const std::string& saveLocation);

/// Whether `path` is on the save location for the take recording now, by
/// path arithmetic only: asking the disk is the very call that hangs on a
/// dead card. A take recorded on this computer for a card (`route`
/// ComputerThenCard) has nothing on the card until it is copied; one recorded
/// straight to the save location has everything there but its backup copy in
/// `backupFolder` (empty when none is kept).
bool isOnTheSaveLocationWhileRecording (const std::string& path, TakeRoute route, const std::string& backupFolder);

/// Said at Stop when the drive the take was being finished on stopped
/// answering. `recordedHere`: the take was recorded on this computer for a
/// card, which it has not touched -- so it is this computer's drive, and its
/// files are named (`takeFolder`), never "check the card". Otherwise the card,
/// with its backup copy on this computer (`backupFolder`, when one was kept).
std::string stoppedAnsweringAtStop (bool recordedHere, const std::string& takeFolder, const std::string& backupFolder);

} // namespace cardcopy
} // namespace mma
