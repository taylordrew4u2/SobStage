#include "TakeCopy.h"

#include "Json.h"
#include "MirrorPolicy.h"
#include "PreflightThroughputTest.h"
#include "SessionFolderNaming.h"
#include "SessionMetadata.h"
#include "SessionRecovery.h"
#include "TakeCompleteness.h"
#include "../Platform/VolumeCapacity.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>

namespace mma {

double computerRecordingSeconds (std::optional<int64_t> freeBytes, double bytesPerSecond) noexcept
{
    // Negative is how the probe says it could not ask. Read as no room, it
    // would stop a take over a disk that only declined to answer.
    if (! freeBytes.has_value() || *freeBytes < 0 || ! (bytesPerSecond > 0.0))
        return -1.0;

    // A take recorded here is stopped with 1 GB still free (§6.5): what this
    // computer itself needs to keep working, and what the copy to the card
    // and the next launch need. Room past that is the take's.
    if (*freeBytes <= MirrorPolicy::kStopBytes)
        return 0.0;

    return static_cast<double> (*freeBytes - MirrorPolicy::kStopBytes) / bytesPerSecond;
}

TakeRoute decideTakeRoute (const TakeRouteInputs& in) noexcept
{
    if (! in.copyAfterTakeEnabled || in.destinationOnThisComputer == 1)
        return TakeRoute::StraightToDestination;

    // Not knowing how much room there is here does not send a take to a card
    // that may be what fails under it.
    if (! in.computerFreeBytes.has_value())
        return TakeRoute::ComputerThenCard;

    const auto need = [&in] (double seconds)
    {
        return static_cast<int64_t> (std::max (0.0, in.bytesPerSecond) * seconds) + MirrorPolicy::kMinHeadroomBytes;
    };

    // An hour here is enough, whatever the card holds: the user chose "record
    // here, then copy", and an hour plus 2 GB is the rule the backup copy has
    // always kept. The card is not asked anything, not even its speed.
    if (*in.computerFreeBytes > need (kPlanningSeconds))
        return TakeRoute::ComputerThenCard;

    // Under an hour, a card that passed its check takes the take, so a long
    // show is not cut short for want of room here. One that refuses -- too
    // slow, full, can't be written, FAT32 with a camera -- would stop the show
    // before it starts, so it never does while there are ten minutes of room
    // here: the take records here and stops short if it must, warned first.
    // Record pressed before the card could be checked is the same: there is
    // no time left to check it.
    const bool cardRefuses = in.card == CardCheck::Refuses || (in.arming && in.card == CardCheck::NotYet);

    // Nor is the show given to a card with less room than this computer: a
    // card left nearly full by earlier shows would stop it sooner than
    // recording here would, whatever its speed.
    const bool cardShorter = in.cardRoomSeconds.has_value() && *in.cardRoomSeconds >= 0.0
                          && *in.cardRoomSeconds < computerRecordingSeconds (in.computerFreeBytes,
                                                                             in.bytesPerSecond);

    if ((cardRefuses || cardShorter) && *in.computerFreeBytes > need (kShortestTakeHereSeconds))
        return TakeRoute::ComputerThenCard;

    return TakeRoute::StraightToCardComputerFull;
}

bool speedTestDue (const TakeRouteInputs& in, bool checked, bool retryDue) noexcept
{
    if (checked && ! retryDue)
        return false;

    if (decideTakeRoute (in) != TakeRoute::ComputerThenCard)
        return true;

    if (! checked)
        return false;

    auto passed = in;
    passed.card = CardCheck::Clears;
    return decideTakeRoute (passed) == TakeRoute::StraightToCardComputerFull;
}

std::string SpeedTestTurn::waitingReason (bool copyStalled)
{
    if (copyStalled)
        return "The card stopped answering while a take was being copied to it, so its speed can't be checked. "
               "Check it's plugged in, or choose another place to save.";

    return "Pausing the copy to the card to check this card is fast enough...";
}

namespace cardcopy {

namespace {

constexpr const char* kSessionRecord = "session.json";
constexpr const char* kActivityLog = "activity.log";

bool isSeparator (char c) noexcept
{
    return c == '/' || c == '\\';
}

/// The take's two record files: rewritten on the card when they differ,
/// because they are records of the take rather than the take. Audio and
/// video never are.
bool isRecordFile (const std::string& name)
{
    return name == kSessionRecord || name == kActivityLog;
}

std::string verifiedKey (const std::string& cardPath, uint64_t size)
{
    return cardPath + "|" + std::to_string (size);
}

bool cancelled (const Control& control)
{
    return control.cancel != nullptr && control.cancel->load (std::memory_order_acquire);
}

bool yielding (const Control& control)
{
    return control.shouldYield && control.shouldYield();
}

/// Cancelled or Yielded when the copy must stop here, before its next call to
/// the card; nothing otherwise.
std::optional<Outcome> stopRequested (const Control& control)
{
    if (cancelled (control))
        return Outcome::Cancelled;

    if (yielding (control))
        return Outcome::Yielded;

    return std::nullopt;
}

/// Fills `buffer` as far as the file allows: a read may hand back less than
/// was asked without being at the end.
FsResult readFully (CopyFileSystem::Reader& reader, unsigned char* buffer, size_t capacity, size_t& got)
{
    got = 0;

    while (got < capacity)
    {
        size_t n = 0;
        const auto r = reader.read (buffer + got, capacity - got, n);

        if (! r.ok())
            return r;

        if (n == 0)
            break;

        got += n;
    }

    return {};
}

/// A card write, folder or rename that failed, in the user's terms. A card
/// that is no longer there is "missing" whatever error the write gave: a card
/// pulled mid-write answers with all sorts.
Outcome outcomeForCardError (CopyFileSystem& fs, const std::string& stillThere, const FsResult& r)
{
    if (! fs.exists (stillThere))
        return Outcome::CardMissing;

    switch (r.error)
    {
        case FsError::NoSpace:    return Outcome::CardFull;
        case FsError::ReadOnly:   return Outcome::CardLocked;
        case FsError::NotAllowed: return Outcome::NotAllowed;
        case FsError::TooBig:     return Outcome::FileTooBig;
        case FsError::None:
        case FsError::NotFound:
        case FsError::Other:
        default:                  return Outcome::CardWriteFailed;
    }
}

enum class Comparison { Match, Differ, LocalFailed, CardFailed, Yielded, Cancelled };

struct CompareResult
{
    Comparison verdict = Comparison::Differ;
    FsResult error;
};

/// Byte for byte, the card side read past this computer's cache, so what is
/// compared is what the card actually holds.
CompareResult compareWithCard (CopyFileSystem& fs, const std::string& localPath, const std::string& cardPath,
                               Control& control, std::vector<unsigned char>& a, std::vector<unsigned char>& b,
                               const std::function<void()>& tick)
{
    FsResult r;
    auto local = fs.openRead (localPath, false, r);
    if (local == nullptr)
        return { Comparison::LocalFailed, r };

    auto card = fs.openRead (cardPath, true, r);
    if (card == nullptr)
        return { Comparison::CardFailed, r };

    for (;;)
    {
        if (cancelled (control))
            return { Comparison::Cancelled, {} };

        if (yielding (control))
            return { Comparison::Yielded, {} };

        size_t gotLocal = 0, gotCard = 0;

        if (const auto lr = readFully (*local, a.data(), a.size(), gotLocal); ! lr.ok())
            return { Comparison::LocalFailed, lr };

        if (const auto cr = readFully (*card, b.data(), b.size(), gotCard); ! cr.ok())
            return { Comparison::CardFailed, cr };

        if (gotLocal != gotCard || std::memcmp (a.data(), b.data(), gotLocal) != 0)
            return { Comparison::Differ, {} };

        if (gotLocal == 0)
            return { Comparison::Match, {} };

        // Still moving: reading a big file back is not a card that has
        // stopped answering.
        tick();
    }
}

Result finish (Result result, Outcome outcome, const std::string& fileName = {}, int osError = 0)
{
    result.outcome = outcome;
    result.fileName = fileName;
    result.osError = osError;
    return result;
}

} // namespace

std::string markerToJson (const Marker& marker)
{
    JsonValue root = JsonValue::makeObject();
    root["destinationRoot"] = JsonValue (marker.destinationRoot);
    root["cardFolder"] = JsonValue (marker.cardFolder);
    root["cardIdentity"] = JsonValue (marker.cardIdentity);
    if (marker.takeNextCard)
        root["takeNextCard"] = JsonValue (true);
    return root.dump (2);
}

std::optional<Marker> markerFromJson (const std::string& text)
{
    try
    {
        const auto parsed = JsonValue::parse (text);
        const auto* root = parsed.find ("destinationRoot");

        if (root == nullptr || root->getType() != JsonValue::Type::String || root->asString().empty())
            return std::nullopt;

        Marker marker;
        marker.destinationRoot = root->asString();

        if (const auto* folder = parsed.find ("cardFolder"))
            marker.cardFolder = folder->asString();

        // Absent from a marker an earlier version wrote: not known.
        if (const auto* identity = parsed.find ("cardIdentity");
            identity != nullptr && identity->getType() == JsonValue::Type::String)
            marker.cardIdentity = identity->asString();

        if (const auto* nextCard = parsed.find ("takeNextCard"))
            marker.takeNextCard = nextCard->asBool (false);

        return marker;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool isWorkingFileName (const std::string& name)
{
    const std::string suffix = kWorkingSuffix;

    return name.size() > 1 + suffix.size() && name.front() == '.'
        && name.compare (name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool shouldCopy (const std::string& name)
{
    return ! name.empty() && ! isSystemClutterFile (name) && ! isAbandonedTakeWorkingFile (name)
        && name != kMarkerFileName;
}

std::vector<std::string> copyOrder (std::vector<std::string> names)
{
    const auto rank = [] (const std::string& name)
    {
        if (name == kSessionRecord) return 2;
        if (name == kActivityLog)   return 1;
        return 0;
    };

    std::sort (names.begin(), names.end(), [&rank] (const std::string& x, const std::string& y)
    {
        const int rx = rank (x), ry = rank (y);
        return rx != ry ? rx < ry : x < y;
    });

    return names;
}

std::string joinPath (const std::string& folder, const std::string& name)
{
    if (folder.empty())
        return name;

    if (isSeparator (folder.back()))
        return folder + name;

    // The separator the folder already uses, so one path never mixes them:
    // a "/" path given on Windows stays a "/" path, which comparisons with
    // the folder's own spelling (a card folder, a marker) rely on.
    for (auto i = folder.size(); i > 0; --i)
        if (isSeparator (folder[i - 1]))
            return folder + folder[i - 1] + name;

    // MMA_TAKECOPY_BACKSLASH_PATHS is the Windows spelling compiled on another
    // system, for mma_takecopy_backslash_tests.
   #if defined(_WIN32) || defined(MMA_TAKECOPY_BACKSLASH_PATHS)
    return folder + "\\" + name;
   #else
    return folder + "/" + name;
   #endif
}

std::string parentOf (const std::string& path)
{
    auto end = path.size();

    while (end > 1 && isSeparator (path[end - 1]))
        --end;

    for (auto i = end; i > 0; --i)
    {
        if (! isSeparator (path[i - 1]))
            continue;

        // "/x" -> "/", "C:\x" -> "C:\".
        if (i == 1 || (i == 3 && path[1] == ':'))
            return path.substr (0, i);

        return path.substr (0, i - 1);
    }

    return path;
}

std::string lastPartOf (const std::string& path)
{
    auto end = path.size();

    while (end > 0 && isSeparator (path[end - 1]))
        --end;

    auto start = end;
    while (start > 0 && ! isSeparator (path[start - 1]))
        --start;

    return path.substr (start, end - start);
}

namespace {

/// What one attempt saw of the take's folder on the card in the reader.
struct CardLook
{
    /// The card was there and its take folder was looked at (or found not
    /// to be there at all): `files` is what that folder holds.
    bool looked = false;
    std::map<std::string, uint64_t> files;
};

Result copyTakeOnce (CopyFileSystem& fs, const std::string& localFolder, const Marker& marker, Control& control,
                     std::set<std::string>& verified, CardLook& look)
{
    Result result;
    result.cardFolder = marker.cardFolder;
    result.cardIdentity = marker.cardIdentity;

    // 1. The take must be finished. A session.json without a stop time is the
    //    take recording now, or one a crash cut off: the recovery card stamps
    //    its stop time, and the copy is tried again after that. A take this
    //    run stopped is finished, whatever its record says.
    if (! fs.exists (localFolder))
        return finish (result, Outcome::ComputerCopyMissing);

    if (! control.takeIsFinished)
    {
        std::string text;

        if (! fs.readText (joinPath (localFolder, kSessionRecord), text).ok())
            return finish (result, Outcome::NotFinishedYet);

        try
        {
            if (SessionRecovery::sessionWasInterrupted (SessionMetadata::fromJsonString (text)))
                return finish (result, Outcome::NotFinishedYet);
        }
        catch (...)
        {
            return finish (result, Outcome::NotFinishedYet);
        }
    }

    // A file on this computer that could not be opened or read: the folder
    // gone (deleted, or renamed in Finder -- the caller follows a rename), or
    // a file busy for a moment with something else, which is tried again.
    const auto computerSide = [&fs, &localFolder, &result] (const std::string& name, const FsResult& r)
    {
        return finish (result, fs.exists (localFolder) ? Outcome::ComputerReadFailed : Outcome::ComputerCopyMissing,
                       name, r.osError);
    };

    // 2. What goes.
    std::vector<Entry> localEntries;
    if (const auto r = fs.list (localFolder, localEntries); ! r.ok())
        return computerSide ({}, r);

    std::map<std::string, uint64_t> localSizes;
    std::vector<std::string> names;

    for (const auto& e : localEntries)
    {
        if (e.isFolder || ! shouldCopy (e.name))
            continue;

        localSizes[e.name] = e.size;
        names.push_back (e.name);
    }

    names = copyOrder (std::move (names));

    const auto markerPath = joinPath (localFolder, kMarkerFileName);

    // Everything already checked on the card in this run of the app: nothing
    // new arrived, so the card is not touched at all. Every key here was put
    // there by an attempt that ended Copied -- a failed attempt forgets its
    // take's keys (copyTake) -- so this only says again what was true then.
    if (! marker.cardFolder.empty()
        && std::all_of (names.begin(), names.end(), [&] (const std::string& name)
           {
               return verified.count (verifiedKey (joinPath (marker.cardFolder, name), localSizes[name])) > 0;
           }))
    {
        if (! control.keepMarker && fs.exists (markerPath))
            (void) fs.remove (markerPath);

        result.filesOnCard = localSizes;
        return finish (result, Outcome::Copied);
    }

    // Not one call to the card once a take has started: the card may be on
    // the microphones' hub, and the take's own writes come first.
    if (const auto stop = stopRequested (control))
        return finish (result, *stop);

    // Something new to copy for a take whose card folder is already chosen --
    // a combined video added after the take was copied, which removed the
    // marker. Remembered on this computer before the card is asked anything,
    // so a card that is out now, or a quit, still has it copied at the next
    // launch.
    const auto rememberOnThisComputer = [&fs, &markerPath] (const Marker& wanted)
    {
        const auto text = markerToJson (wanted);
        std::string onDisk;

        if (! fs.readText (markerPath, onDisk).ok() || onDisk != text)
            (void) fs.replaceText (markerPath, text);
    };

    if (! marker.cardFolder.empty())
        rememberOnThisComputer (marker);

    // 3. The card. Its save folder is made if the card is there and the
    //    folder is not, one level only -- never a missing card's mount point
    //    on the startup disk.
    const auto& root = marker.destinationRoot;

    // Every refusal from the card, from its first folder on: a card that will
    // not be written says what kind it is, so a Windows-formatted drive a Mac
    // mounts read-only is not called a card with its lock switch on.
    const auto refusedOnCard = [&] (Outcome outcome, const std::string& name, const FsResult& r)
    {
        auto failed = finish (result, outcome, name, r.osError);

        if (outcome == Outcome::CardLocked || outcome == Outcome::NotAllowed)
            failed.filesystemType = fs.filesystemTypeName (root);

        return failed;
    };

    if (root.empty() || volumecapacity::isUnderMissingMountPoint (root, [&fs] (const std::string& p) { return fs.exists (p); }))
        return finish (result, Outcome::CardMissing);

    // The save folder, and whichever folders above it are gone too, nearest
    // first. Nothing of it there at all -- not even a drive's root -- is a
    // card that is out.
    std::vector<std::string> missingLevels;

    for (auto p = root; ! fs.exists (p); p = parentOf (p))
    {
        if (parentOf (p) == p || missingLevels.size() >= 64)
            return finish (result, Outcome::CardMissing);

        missingLevels.push_back (p);
    }

    const bool rootThere = missingLevels.empty();
    const auto here = fs.cardIdentity (root);

    // More than the save folder itself is gone: a folder above it deleted on
    // the card ("Shows" of Shows/Fall), or the card reformatted under its
    // old name. Made again only on a card that is there and is not this
    // computer's own disk -- which is what a card that is out answers for, at
    // a place that is not a known mount point. It used to be called "The
    // card isn't plugged in" with the card in the reader, for ever.
    if (missingLevels.size() > 1 && (here.empty() || here == fs.cardIdentity (localFolder)))
        return finish (result, Outcome::CardMissing);

    // A card is there. Which one, before anything is made or written on it:
    // the place a card mounts is not the card. Another one at it -- a drive
    // letter Windows gave a USB stick, a camera card also called "NO NAME" --
    // is left alone, and the take waits for its own card. "Copy again" takes
    // whichever card is in.
    auto cardFolderWanted = marker.cardFolder;

    if (! here.empty() && here != marker.cardIdentity)
    {
        if (! marker.cardIdentity.empty() && ! control.anyCard && ! marker.takeNextCard)
            return finish (result, Outcome::DifferentCard);

        // This card is the take's from now on: in the note on this computer
        // before the card is touched, so a crash or a quit keeps to it. A
        // card taken over by "Copy again" chooses its own folder name.
        if (! marker.cardIdentity.empty())
            cardFolderWanted.clear();

        // Not chosen: the take never knew its card. Said, not taken silently.
        result.cardFirstSeen = marker.cardIdentity.empty() && ! control.anyCard;
        result.cardIdentity = here;
        result.cardFolder = cardFolderWanted;
        rememberOnThisComputer ({ root, cardFolderWanted, here });
    }

    if (! rootThere)
    {
        // A card is there, without even the save folder: none of the take.
        // Made from the top down, one level at a time, below a folder that
        // is there -- never a mount point itself.
        look.looked = true;

        for (auto level = missingLevels.rbegin(); level != missingLevels.rend(); ++level)
            if (const auto r = fs.makeFolder (*level); ! r.ok())
                return refusedOnCard (outcomeForCardError (fs, parentOf (*level), r), {}, r);
    }

    // 4. The take's folder on the card, remembered on this computer BEFORE it
    //    is made: a crash in between then reuses it, rather than leaving an
    //    empty folder behind and copying into "_2".
    auto cardFolder = cardFolderWanted;

    if (cardFolder.empty())
    {
        const auto name = SessionFolderNaming::resolveCollision (
            lastPartOf (localFolder),
            [&fs, &root] (const std::string& candidate) { return fs.exists (joinPath (root, candidate)); });

        cardFolder = joinPath (root, name);
    }

    result.cardFolder = cardFolder;

    // Still waiting to take the next card only while this is the card it
    // was told about: taken, it is the take's card now.
    if (cardFolder != cardFolderWanted)
        rememberOnThisComputer ({ root, cardFolder, result.cardIdentity,
                                  marker.takeNextCard && result.cardIdentity == marker.cardIdentity });

    if (! fs.exists (cardFolder))
    {
        // The card in the reader has no folder for this take: none of it.
        look.looked = true;

        if (const auto r = fs.makeFolder (cardFolder); ! r.ok())
            return refusedOnCard (outcomeForCardError (fs, root, r), {}, r);
    }

    std::vector<Entry> cardEntries;
    if (const auto r = fs.list (cardFolder, cardEntries); ! r.ok())
        return refusedOnCard (outcomeForCardError (fs, root, r), {}, r);

    // 5. Half-written copies left by an attempt that was cut off.
    std::map<std::string, uint64_t> onCard;

    for (const auto& e : cardEntries)
    {
        if (e.isFolder)
            continue;

        if (isWorkingFileName (e.name))
            (void) fs.remove (joinPath (cardFolder, e.name));
        else
            onCard[e.name] = e.size;
    }

    look.looked = true;
    look.files = onCard;

    // A file checked earlier in this run is on the card only while the card
    // still lists it, under its name and at its size. A reader that dropped
    // out and came back, a card whose last folder updates were lost, or
    // another card with the same name mounted in its place: each is read as
    // it is now, and whatever is missing is copied again.
    for (const auto& name : names)
    {
        const auto there = onCard.find (name);

        if (there == onCard.end() || there->second != localSizes[name])
            verified.erase (verifiedKey (joinPath (cardFolder, name), localSizes[name]));
    }

    // 6. Refused before anything is written: a file this card's format cannot
    //    hold, a different file already under one of the take's names, and a
    //    card without room for the rest.
    uint64_t toWrite = 0;
    std::vector<std::string> needWriting;

    for (const auto& name : names)
    {
        const auto size = localSizes[name];

        if (verified.count (verifiedKey (joinPath (cardFolder, name), size)) > 0)
            continue;

        const auto there = onCard.find (name);

        if (there != onCard.end() && there->second == size)
            continue; // compared below

        if (there != onCard.end() && ! isRecordFile (name))
            return finish (result, Outcome::NameTakenOnCard, name);

        toWrite += size;
        needWriting.push_back (name);
    }

    // A file this card's format cannot hold -- a camera's movie past 4 GB on
    // a FAT32 card -- is left on this computer and everything else goes: the
    // sound must not wait on a movie that can never fit. session.json is held
    // back with it, so the card's folder never looks complete without it.
    std::vector<std::string> tooBig;
    std::set<std::string> leftBehind;

    const auto leaveBehind = [&] (const std::string& name)
    {
        tooBig.push_back (name);
        leftBehind.insert (name);
        leftBehind.insert (kSessionRecord);
    };

    if (fs.limitedTo4GiBFiles (cardFolder).value_or (false))
        for (const auto& name : needWriting)
            if (localSizes[name] >= kFat32FileLimitBytes)
                leaveBehind (name);

    if (! tooBig.empty())
    {
        toWrite = 0;
        for (const auto& name : needWriting)
            if (leftBehind.count (name) == 0)
                toWrite += localSizes[name];
    }

    if (const auto free = fs.freeBytes (cardFolder); free.has_value() && toWrite > 0
        && *free < toWrite + kCardMarginBytes)
        return finish (result, Outcome::CardFull);

    // 7. Each file: written under its working name, flushed, read back past
    //    the cache and matched, and only then given its real name.
    std::vector<unsigned char> bufferA (kChunkBytes), bufferB (kChunkBytes);
    uint64_t done = 0;

    // A file found already on the card under its real name was renamed by an
    // attempt that may have been cut off before its folder was flushed.
    bool folderNeedsSync = false;

    const std::function<void()> tick = [&]
    {
        if (control.progress)
            control.progress (done, toWrite);
    };

    for (const auto& name : names)
    {
        const auto size = localSizes[name];
        const auto localPath = joinPath (localFolder, name);
        const auto cardPath = joinPath (cardFolder, name);
        const auto key = verifiedKey (cardPath, size);

        if (verified.count (key) > 0 || leftBehind.count (name) > 0)
            continue;

        if (const auto stop = stopRequested (control))
            return finish (result, *stop);

        const auto there = onCard.find (name);

        // Already on the card under its real name: a copy that finished before
        // a quit or crash, checked rather than trusted.
        if (there != onCard.end() && there->second == size)
        {
            const auto same = compareWithCard (fs, localPath, cardPath, control, bufferA, bufferB, tick);

            switch (same.verdict)
            {
                case Comparison::Match:
                    verified.insert (key);
                    folderNeedsSync = true;
                    continue;
                case Comparison::Cancelled:   return finish (result, Outcome::Cancelled);
                case Comparison::Yielded:     return finish (result, Outcome::Yielded);
                case Comparison::LocalFailed: return computerSide (name, same.error);
                case Comparison::CardFailed:
                    return refusedOnCard (outcomeForCardError (fs, root, same.error), name, same.error);
                case Comparison::Differ:
                    break;
            }

            // Never over audio: a different MIX.wav on the card is someone's
            // recording, and this copy's is safe on the computer either way.
            if (! isRecordFile (name))
                return finish (result, Outcome::NameTakenOnCard, name);
        }

        const auto workingPath = joinPath (cardFolder, "." + name + kWorkingSuffix);
        bool matched = false;
        bool refusedAsTooBig = false;

        for (int attempt = 0; attempt < 2 && ! matched && ! refusedAsTooBig; ++attempt)
        {
            const auto doneBefore = done;
            FsResult r;

            auto source = fs.openRead (localPath, false, r);
            if (source == nullptr)
                return computerSide (name, r);

            auto writer = fs.createNew (workingPath, r);
            if (writer == nullptr)
                return refusedOnCard (outcomeForCardError (fs, root, r), name, r);

            const auto abandon = [&] (std::unique_ptr<CopyFileSystem::Writer>& w)
            {
                w.reset();
                (void) fs.remove (workingPath);
            };

            for (;;)
            {
                if (cancelled (control))
                {
                    abandon (writer);
                    return finish (result, Outcome::Cancelled);
                }

                if (yielding (control))
                {
                    abandon (writer);
                    return finish (result, Outcome::Yielded);
                }

                size_t got = 0;
                if (const auto rr = readFully (*source, bufferA.data(), bufferA.size(), got); ! rr.ok())
                {
                    abandon (writer);
                    return computerSide (name, rr);
                }

                if (got == 0)
                    break;

                if (const auto wr = writer->write (bufferA.data(), got); ! wr.ok())
                {
                    auto outcome = outcomeForCardError (fs, root, wr);
                    abandon (writer);

                    // Windows' FAT driver refuses a write past 4 GB as a full
                    // disk (ERROR_DISK_FULL), never as a file too large, and so
                    // does a FAT drive shared by a server that says "NTFS". A
                    // card "full" exactly at the 4 GB mark while it still has
                    // room for the rest of the take is a file too big for it --
                    // the sound must not stay off the card for a movie, nor the
                    // user be told a card with room on it is full.
                    if (outcome == Outcome::CardFull && (done - doneBefore) + got >= kFat32FileLimitBytes)
                    {
                        const auto stillToWrite = toWrite > doneBefore ? toWrite - doneBefore : 0;

                        if (const auto free = fs.freeBytes (cardFolder); ! free.has_value() || *free >= stillToWrite)
                            outcome = Outcome::FileTooBig;
                    }

                    // The card's format said so only now: the same as one
                    // known to be FAT32 before writing. The rest still goes.
                    if (outcome == Outcome::FileTooBig)
                    {
                        refusedAsTooBig = true;
                        break;
                    }

                    return refusedOnCard (outcome, name, wr);
                }

                done += got;
                result.bytesCopied += got;
                tick();
            }

            if (refusedAsTooBig)
            {
                done = doneBefore;
                break;
            }

            if (const auto sr = writer->syncAndClose(); ! sr.ok())
            {
                const auto outcome = outcomeForCardError (fs, root, sr);
                abandon (writer);
                return refusedOnCard (outcome, name, sr);
            }

            writer.reset();

            const auto check = compareWithCard (fs, localPath, workingPath, control, bufferA, bufferB, tick);

            switch (check.verdict)
            {
                case Comparison::Match:
                    matched = true;
                    break;
                case Comparison::Cancelled:
                    (void) fs.remove (workingPath);
                    return finish (result, Outcome::Cancelled);
                case Comparison::Yielded:
                    (void) fs.remove (workingPath);
                    return finish (result, Outcome::Yielded);
                case Comparison::LocalFailed:
                    (void) fs.remove (workingPath);
                    return computerSide (name, check.error);
                case Comparison::CardFailed:
                {
                    const auto outcome = outcomeForCardError (fs, root, check.error);
                    (void) fs.remove (workingPath);
                    return refusedOnCard (outcome, name, check.error);
                }
                case Comparison::Differ:
                    // Removed, and written once more: a card that garbled one
                    // write may take the next. Twice is a card that cannot be
                    // trusted with this take.
                    (void) fs.remove (workingPath);
                    done = doneBefore;
                    break;
            }
        }

        if (refusedAsTooBig)
        {
            leaveBehind (name);
            continue;
        }

        if (! matched)
            return finish (result, Outcome::CopyDidNotMatch, name);

        if (const auto r = fs.rename (workingPath, cardPath, isRecordFile (name)); ! r.ok())
        {
            const auto outcome = outcomeForCardError (fs, root, r);
            (void) fs.remove (workingPath);
            return refusedOnCard (outcome, name, r);
        }

        // The new name on the card itself before the file counts as copied:
        // a card pulled without ejecting can otherwise come back with only
        // the hidden working file, which the next attempt deletes.
        if (const auto r = fs.syncFolder (cardFolder); ! r.ok())
            return refusedOnCard (outcomeForCardError (fs, root, r), name, r);

        verified.insert (key);
        look.files[name] = size;
        ++result.filesCopied;
    }

    if (folderNeedsSync)
        if (const auto r = fs.syncFolder (cardFolder); ! r.ok())
            return refusedOnCard (outcomeForCardError (fs, root, r), {}, r);

    // Everything that fits is on the card; what does not stays here, with the
    // marker, and is said.
    if (! tooBig.empty())
        return finish (result, Outcome::FileTooBig, tooBig.front());

    // 8. Everything is on the card and checked. The marker going is all that
    //    changes on this computer; if it will not go, the next launch checks
    //    the card again by comparing and tries once more. Kept while a file
    //    is still being made in the take's folder.
    if (! control.keepMarker)
        (void) fs.remove (markerPath);

    result.filesOnCard = localSizes;
    return finish (result, Outcome::Copied);
}

} // namespace

Result copyTake (CopyFileSystem& fs, const std::string& localFolder, const Marker& marker, Control& control)
{
    std::set<std::string> scratchVerified;
    auto& verified = control.verified != nullptr ? *control.verified : scratchVerified;

    CardLook look;
    auto result = copyTakeOnce (fs, localFolder, marker, control, verified, look);

    // A file too big for this card: the user is told to use another card or
    // reformat this one, and the next card reached is taken as the take's.
    // Said in the note on this computer, so a quit before that card is in --
    // the usual moment a card is reformatted -- keeps to it; and taken back
    // once a card is taken or the take is copied.
    {
        const auto markerPath = joinPath (localFolder, kMarkerFileName);
        const auto taken = ! result.cardIdentity.empty() && result.cardIdentity != marker.cardIdentity;
        std::optional<Marker> rewrite;

        if (result.outcome == Outcome::FileTooBig)
            rewrite = Marker { marker.destinationRoot, result.cardFolder, result.cardIdentity, true };
        else if (marker.takeNextCard && (taken || result.outcome == Outcome::Copied))
            rewrite = Marker { marker.destinationRoot, result.cardFolder, result.cardIdentity, false };

        // Written for a file too big even where the note was missing -- the
        // take is still to go to a card; only rewritten where it is there
        // otherwise, a copied take's having gone.
        std::string onDisk;
        const bool there = rewrite.has_value() && fs.readText (markerPath, onDisk).ok();

        if (rewrite.has_value() && (there || result.outcome == Outcome::FileTooBig)
            && onDisk != markerToJson (*rewrite))
            (void) fs.replaceText (markerPath, markerToJson (*rewrite));
    }

    // Only a file added since is missing when every file an earlier attempt
    // copied is on the card this attempt looked at -- the records by name,
    // as they are rewritten. A card that could not be looked at (out, or
    // refused before its take folder was reached) is the one that was
    // checked; another card at the same place, or one emptied, is not.
    if (control.copiedBefore != nullptr && ! control.copiedBefore->empty())
        result.addedFilesOnly = ! look.looked
            || std::all_of (control.copiedBefore->begin(), control.copiedBefore->end(),
                            [&look] (const std::pair<const std::string, uint64_t>& copied)
                            {
                                const auto there = look.files.find (copied.first);
                                return there != look.files.end()
                                    && (isRecordFile (copied.first) || there->second == copied.second);
                            });

    // A card that cannot be trusted with what it held a moment ago -- pulled,
    // swapped, garbled, or holding files that are not this take's -- has
    // every file of this take checked against it again next time. A full or
    // locked card, a file too big for its format, or a failure on this
    // computer's side says nothing about what is on it.
    const bool cardInDoubt = result.outcome == Outcome::CardMissing
                          || result.outcome == Outcome::DifferentCard
                          || result.outcome == Outcome::CardWriteFailed
                          || result.outcome == Outcome::CopyDidNotMatch
                          || result.outcome == Outcome::NameTakenOnCard;

    if (cardInDoubt)
    {
        for (const auto& folder : { marker.cardFolder, result.cardFolder })
        {
            if (folder.empty())
                continue;

            const auto prefix = joinPath (folder, {});

            for (auto it = verified.begin(); it != verified.end();)
                it = it->compare (0, prefix.size(), prefix) == 0 ? verified.erase (it) : std::next (it);
        }
    }

    return result;
}

std::string tallyLine (const Tally& tally)
{
    const auto takes = [] (int n, const char* one, const char* many)
    {
        return n == 1 ? std::string ("1 take ") + one : std::to_string (n) + " takes " + many;
    };

    if (tally.onlyOnComputer > 0)
    {
        // "When it is back" only when that is the reason: a full or locked
        // card is not coming back by itself, and its saved-take card says
        // what to do.
        if (tally.cardMissing == tally.onlyOnComputer)
            return takes (tally.onlyOnComputer, "is", "are") + " only on this computer. "
                 + (tally.onlyOnComputer == 1 ? "It" : "They") + " will be copied to the card when it is back.";

        // The card in is not theirs: "when it is back" would read as this one.
        if (tally.differentCard == tally.onlyOnComputer)
            return tally.onlyOnComputer == 1
                ? std::string ("1 take is only on this computer: the card in now isn't the one it's for. "
                               "It will be copied when its own card is back.")
                : std::to_string (tally.onlyOnComputer) + " takes are only on this computer: the card in now "
                  "isn't the one they're for. They will be copied when their own card is back.";

        return takes (tally.onlyOnComputer, "is", "are") + " only on this computer for now.";
    }

    if (tally.partlyOnCard > 0)
        return takes (tally.partlyOnCard, "has a file", "have files")
             + " too big for this card, kept on this computer. The rest is on the card.";

    if (tally.addedFilesWaiting > 0)
        return takes (tally.addedFilesWaiting, "has a file", "have files") + " still to copy to the card.";

    return {};
}

namespace {

bool isSeparatorChar (char c) noexcept
{
    return c == '/' || c == '\\';
}

/// `path` is `folder` or inside it, by spelling.
bool isSameOrInside (const std::string& path, const std::string& folder)
{
    auto base = folder;
    while (base.size() > 1 && isSeparatorChar (base.back()))
        base.pop_back();

    if (base.empty() || path.compare (0, base.size(), base) != 0)
        return false;

    return path.size() == base.size() || isSeparatorChar (path[base.size()]) || isSeparatorChar (base.back());
}

} // namespace

std::string cardForTake (const std::string& seenNow, const std::map<std::string, std::string>& lastSeen,
                         const std::string& saveLocation)
{
    if (! seenNow.empty())
        return seenNow;

    const auto found = lastSeen.find (saveLocation);
    return found != lastSeen.end() ? found->second : std::string();
}

bool isOnTheTakesDrive (const std::string& path, const std::string& backupFolder)
{
    return backupFolder.empty() || ! isSameOrInside (path, backupFolder);
}

std::string stoppedAnsweringAtStop (bool recordedHere, const std::string& takeFolder, const std::string& backupFolder)
{
    if (recordedHere)
        return "This computer's drive stopped answering while this take was being finished, so the end of the "
               "recording may be missing from it. Its files are in " + takeFolder + ".";

    return "The card stopped answering while this take was being finished, so the end of the recording may be "
           "missing from it."
         + (! backupFolder.empty() ? " The backup copy on this computer may be missing the same part: " + backupFolder
                                   : std::string (" Check the card before recording again."));
}

std::string stoppedAnsweringStatusLine (bool recordedHere, const std::string& takeFolder,
                                        const std::string& backupFolder)
{
    // The card's line sends the user to check the card; a take recorded here
    // never touched it, and the drive that failed would take the next take.
    if (recordedHere)
        return "Recording stopped, but this computer's drive stopped answering, so the end of the take may be "
               "missing from " + takeFolder + ".";

    return "Recording stopped, but the card stopped answering, so the end of the take may be missing from "
         + takeFolder
         + (! backupFolder.empty() ? ". The backup copy is at " + backupFolder + "."
                                   : std::string (". Check the card before recording again."));
}

std::string stopTimedOutAccount (bool recordedHere, const std::string& coordinatorAccount)
{
    if (recordedHere)
        return "This computer's drive stopped answering while the take was being finished, so the end of the "
               "recording may not have been saved.";

    return coordinatorAccount;
}

std::string notMadeAfterStopTimedOut (bool recordedHere, bool podcast)
{
    const std::string what = podcast ? "No podcast-ready copy was made: " : "No combined video was made: ";

    if (recordedHere)
        return what + "this computer's drive stopped answering."
             + (podcast ? std::string() : std::string (" Whatever was saved is still in the take's separate files."));

    return what + "the card stopped answering."
         + (podcast ? std::string() : std::string (" Whatever reached the card is still in its separate files."));
}

std::string stoppedAcceptingWritesReason (bool recordedHere)
{
    return recordedHere ? "this computer's drive stopped accepting writes" : "the card stopped accepting writes";
}

std::string copyFailedBeforeQuitting (const std::vector<std::pair<Result, std::string>>& failures, Wording wording)
{
    if (failures.empty())
        return {};

    if (failures.size() == 1)
        return "The copy to the card didn't finish. "
             + describe (failures.front().first, failures.front().second, wording, Moment::Quitting);

    std::string text = "The copies of " + std::to_string (failures.size()) + " takes to the card didn't finish.";

    for (const auto& [result, folder] : failures)
        text += "\n\n" + lastPartOf (folder) + ": " + describe (result, folder, wording, Moment::Quitting);

    return text;
}

void FailuresWhileQuitting::startWatching()
{
    if (watching)
        return;

    watching = true;
    kept.clear();
}

void FailuresWhileQuitting::stopWatching()
{
    watching = false;
    kept.clear();
}

void FailuresWhileQuitting::noteFinished (const std::string& localFolder, const std::string& folderNow,
                                          const Result& result)
{
    // On the card after all -- the next Stop's retry, or the card put back
    // while the quit waited for something else: nothing to ask about.
    if (result.outcome == Outcome::Copied)
    {
        kept.erase (std::remove_if (kept.begin(), kept.end(),
                                    [&localFolder] (const Kept& k) { return k.localFolder == localFolder; }),
                    kept.end());
        return;
    }

    if (! watching || ! isFailure (result.outcome))
        return;

    // The take's latest attempt replaces its earlier one, told or not: failing
    // again after the user was told is news again.
    kept.erase (std::remove_if (kept.begin(), kept.end(),
                                [&localFolder] (const Kept& k) { return k.localFolder == localFolder; }),
                kept.end());
    kept.push_back ({ localFolder, folderNow, result, false });
}

std::vector<std::pair<Result, std::string>> FailuresWhileQuitting::untold() const
{
    std::vector<std::pair<Result, std::string>> out;

    for (const auto& k : kept)
        if (! k.told)
            out.emplace_back (k.result, k.folderNow);

    return out;
}

void FailuresWhileQuitting::told() noexcept
{
    for (auto& k : kept)
        k.told = true;
}

namespace {

/// " Keep A: it isn't all on the card yet.", naming each take in
/// `stillToCopy` once, or nothing for none.
std::string keepClause (const std::vector<std::string>& stillToCopy)
{
    // By name: a count told nobody which folder to keep, and in Finder the
    // one not on the card looks like every other.
    std::vector<std::string> names;
    for (const auto& folder : stillToCopy)
        if (const auto name = lastPartOf (folder); ! name.empty()
            && std::find (names.begin(), names.end(), name) == names.end())
            names.push_back (name);

    if (names.size() == 1)
        return " Keep " + names.front() + ": it isn't all on the card yet.";

    if (names.empty())
        return {};

    std::string list;
    for (size_t i = 0; i < names.size(); ++i)
        list += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];

    return " Keep " + list + ": they aren't all on the card yet.";
}

/// "about 1h 40m", "about 25 minutes": floored to the minute, so it never
/// promises a minute this computer does not have.
std::string aboutDuration (double seconds)
{
    const auto minutes = static_cast<int64_t> (std::max (0.0, seconds) / 60.0);

    if (minutes >= 60)
    {
        const auto rest = minutes % 60;
        return "about " + std::to_string (minutes / 60) + "h " + (rest < 10 ? "0" : "") + std::to_string (rest) + "m";
    }

    if (minutes == 0)
        return "less than a minute";

    return "about " + std::to_string (minutes) + (minutes == 1 ? " minute" : " minutes");
}

} // namespace

std::string computerFullNote (const std::string& computerFolder, const std::vector<std::string>& takesStillToCopy,
                              bool nextTake)
{
    return std::string (nextTake ? "This computer is nearly full, so the next take will go straight to the card."
                                 : "This computer is nearly full, so this take is going straight to the card.")
         + " To make room, delete takes in " + computerFolder + " that have already been copied to the card."
         + keepClause (takesStillToCopy);
}

std::string roomHereNote (double hereSeconds, double cardSeconds, bool camerasOn, bool cardRefused)
{
    if (hereSeconds < 0.0)
        return {};

    // Only when this computer runs out first: a card that could hold more is
    // the only reason the number matters, and no show runs past the cap. A
    // card that refused cannot take the show instead, so then it is always
    // said -- it is all the time there is.
    const double cardCould = std::min (cardSeconds >= 0.0 ? cardSeconds : std::numeric_limits<double>::infinity(),
                                       kLongestShowSeconds);

    if (! cardRefused && hereSeconds >= cardCould)
        return {};

    const auto room = "This computer has room for " + aboutDuration (hereSeconds) + " of recording.";
    const std::string more = std::string ("For more time, free up space on this computer")
                           + (camerasOn ? ", or choose a smaller picture size in Cameras." : ".");

    if (cardRefused)
        return "This card can't be recorded on directly, so each take is recorded on this computer, then "
               "copied to the card. " + room + " " + more;

    return room + " Each take is recorded here, then copied to the card. " + more;
}

std::string computerFullRefusal (const std::string& cardReason, const std::string& computerFolder,
                                 const std::vector<std::string>& stillToCopy, CardCheck card)
{
    // "Checking this drive is fast enough..." with no verdict yet: a slow
    // card, the very one this is for, then sends the take here, so saying it
    // would go to the card -- with 50 minutes of room here -- was wrong twice.
    if (card == CardCheck::NotYet)
        return cardReason;

    // Why the card is in it at all: the user chose to record here.
    auto text = "This computer is nearly full, so this take would go straight to the card. " + cardReason;

    if (card == CardCheck::Refuses)
        text += " To record on this computer instead, delete takes in " + computerFolder
              + " that are already on the card." + keepClause (stillToCopy);

    return text;
}

std::string recordRefusal (const std::function<std::string()>& askCard, const std::function<TakeRoute()>& route,
                           const std::function<CardCheck()>& card, const std::string& computerFolder,
                           const std::vector<std::string>& stillToCopy)
{
    const auto reason = askCard();
    const auto now = route();

    if (now == TakeRoute::ComputerThenCard)
        return {};

    if (now != TakeRoute::StraightToCardComputerFull || reason.empty())
        return reason;

    return computerFullRefusal (reason, computerFolder, stillToCopy, card());
}

bool isFailure (Outcome outcome) noexcept
{
    return outcome != Outcome::Copied && outcome != Outcome::Yielded && outcome != Outcome::Cancelled;
}

std::string stalledLine (const std::string& localFolder)
{
    return "The card stopped answering. This take is safe on this computer in " + localFolder + ".";
}

std::string copyStalledLine (const std::string& localFolder)
{
    return "The copy to the card has stopped moving: the card may have stopped answering. This take is safe "
           "on this computer in " + localFolder + ".";
}

std::string waitingLine (int takes)
{
    return takes == 1 ? std::string ("Waiting to copy 1 take to the card.")
                      : "Waiting to copy " + std::to_string (takes) + " takes to the card.";
}

std::string notAnsweringLine (int takes, const std::string& computerFolder)
{
    if (takes == 1)
        return "The card isn't answering, so 1 take is waiting to be copied to it. It's safe on this "
               "computer in " + computerFolder + ".";

    return "The card isn't answering, so " + std::to_string (takes) + " takes are waiting to be copied "
           "to it. They're safe on this computer in " + computerFolder + ".";
}

std::string copyingLine (uint64_t done, uint64_t total)
{
    uint64_t percent = 0;

    if (total > 0)
        percent = std::min<uint64_t> (100, (done >= total ? 100 : (done * 100) / total));

    return "Copying to the card: " + std::to_string (percent) + "%.";
}

namespace {

std::string describeWholeTake (const Result& result, const std::string& localFolder, Wording wording,
                               Moment moment)
{
    const auto safe = " This take is safe on this computer in " + localFolder + ".";
    const auto kind = PreflightThroughputTest::filesystemKindFromTypeName (result.filesystemType);
    const bool quitting = moment == Moment::Quitting;

    switch (result.outcome)
    {
        case Outcome::Copied:
            if (result.cardFirstSeen)
                return "Copied to the card that's in now: " + result.cardFolder + ". SobStage didn't know which "
                       "card this take was for, so check this is the one you meant.";

            return "Copied to the card: " + result.cardFolder + ".";
        case Outcome::NotFinishedYet:
            return "This take didn't finish recording, so it hasn't been copied to the card yet."
                   + safe;
        case Outcome::ComputerCopyMissing:
            return "This take's folder on this computer is gone, so it couldn't be copied to the card.";
        case Outcome::ComputerReadFailed:
            return (result.fileName.empty() ? std::string ("A file") : result.fileName)
                   + " couldn't be read on this computer just now, so the copy to the card stopped. "
                   + (quitting ? "It will be tried again the next time you open SobStage."
                               : "It will be tried again.")
                   + safe;
        case Outcome::CardMissing:
            if (quitting)
                return "The card isn't plugged in." + safe
                     + " It will be copied the next time you open SobStage with the card in.";

            return "The card isn't plugged in. This take is safe on this computer in " + localFolder
                   + ", and will be copied when the card is back.";
        case Outcome::DifferentCard:
            if (quitting)
                return "The card in now isn't the one this take is for, so nothing was copied to it." + safe
                     + " It will be copied the next time you open SobStage with its own card in.";

            return "The card in now isn't the one this take is for, so nothing was copied to it. This take "
                   "is safe on this computer in " + localFolder + ", and will be copied when its own card "
                   "is back. To copy it to the card in now, choose " + std::string (kCopyToThisCard)
                 + ".";
        case Outcome::CardFull:
            return "The card is full." + safe;
        case Outcome::CardLocked:
            // Windows writes NTFS: a drive it will not write is write-protected
            // whatever its format -- a lock switch, or a policy such as
            // BitLocker's on removable drives. The Mac's "reformat it in Disk
            // Utility" would send someone to erase a drive with takes on it.
            if (wording == Wording::Windows)
                return "Windows says this card or drive is write-protected, so the take couldn't be copied "
                       "to it. Slide its lock switch off if it has one. If it doesn't, a Windows setting such "
                       "as BitLocker may be blocking removable drives; or choose a different place to save."
                       + safe;

            return PreflightThroughputTest::writeFailureReason (EROFS, kind) + safe;
        case Outcome::NotAllowed:
            // Not the Mac's privacy settings: Windows refuses with "access
            // denied" for a read-only card and for Controlled folder access.
            if (wording == Wording::Windows)
                return "SobStage isn't allowed to write to this card. Check the card isn't set to read-only, "
                       "and that Controlled folder access in Windows Security allows SobStage, or choose a "
                       "different place to save." + safe;

            return PreflightThroughputTest::writeFailureReason (EACCES, kind) + safe;
        case Outcome::FileTooBig:
            // Another card first, and why not this one reformatted: erasing
            // it erases every take already copied to it. Asked to, a card
            // reformatted with SobStage open has those takes copied to it
            // again (CardCopier), but never one reformatted after a quit.
            return (result.fileName.empty() ? std::string ("A file") : result.fileName)
                   + " is too big for the way this card is formatted, so it wasn't copied. The rest of the "
                     "take was. To copy it, put in another card formatted as ExFAT, and the whole take is "
                     "copied to that one. Don't reformat this card to make it fit: that erases the takes "
                     "already on it." + safe;
        case Outcome::NameTakenOnCard:
            return "The card already has a different file called " + result.fileName + "." + safe;
        case Outcome::CopyDidNotMatch:
            return "The copy on the card didn't match the recording, so it was removed." + safe;
        case Outcome::CardWriteFailed:
            return "The card couldn't be written to." + safe;
        case Outcome::Yielded:
        case Outcome::Cancelled:
        default:
            return {};
    }
}

} // namespace

std::string describe (const Result& result, const std::string& localFolder, Wording wording, Moment moment)
{
    const bool quitting = moment == Moment::Quitting;

    // The take itself already went: what is missing is only what was added
    // to its folder since. "Only on this computer" would be untrue of it.
    if (result.addedFilesOnly && isFailure (result.outcome))
    {
        // Gone to its own card, and another is in the reader now: "This take
        // is on the card" would be read of the one in hand, which holds none
        // of it.
        if (result.outcome == Outcome::DifferentCard)
            return "This take is on its own card, not the one in now. A file added to it since will be copied "
                   + std::string (quitting ? "the next time you open SobStage with that card in."
                                           : "when that card is back.")
                   + " It's safe on this computer in " + localFolder + "."
                   + (quitting ? std::string() : " To copy the whole take to the card in now, choose "
                                                     + std::string (kCopyToThisCard) + ".");

        if (result.outcome == Outcome::CardMissing)
            return "This take is on the card, but a file added to it since isn't yet: the card isn't "
                   "plugged in. It's safe on this computer in " + localFolder
                   + (quitting ? ", and will be copied the next time you open SobStage with the card in."
                               : ", and will be copied when the card is back.");

        return "This take is on the card, but a file added to it since isn't yet. "
               + describeWholeTake (result, localFolder, wording, moment);
    }

    return describeWholeTake (result, localFolder, wording, moment);
}

} // namespace cardcopy
} // namespace mma
