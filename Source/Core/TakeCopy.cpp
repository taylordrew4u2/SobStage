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
#include <map>

namespace mma {

int64_t projectedTakeBytes (const TakeRouteInputs& in) noexcept
{
    const double seconds = in.destinationRemainingSeconds.has_value() && *in.destinationRemainingSeconds >= 0.0
        ? std::min (*in.destinationRemainingSeconds, kLongestShowSeconds)
        : kPlanningSeconds;

    return static_cast<int64_t> (std::max (0.0, in.bytesPerSecond) * seconds);
}

TakeRoute decideTakeRoute (const TakeRouteInputs& in) noexcept
{
    if (! in.copyAfterTakeEnabled || in.destinationOnThisComputer == 1)
        return TakeRoute::StraightToDestination;

    // The same rule the backup copy has always had: room for the take plus
    // 2 GB to spare. A computer that fills up mid-take stops the take, so one
    // that cannot hold what the card could records straight to the card,
    // which is how every take was recorded before this.
    if (in.computerFreeBytes.has_value()
        && *in.computerFreeBytes <= projectedTakeBytes (in) + MirrorPolicy::kMinHeadroomBytes)
        return TakeRoute::StraightToCardComputerFull;

    return TakeRoute::ComputerThenCard;
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

   #if defined(_WIN32)
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

Result copyTakeOnce (CopyFileSystem& fs, const std::string& localFolder, const Marker& marker, Control& control,
                     std::set<std::string>& verified)
{
    Result result;
    result.cardFolder = marker.cardFolder;

    // 1. The take must be finished. A session.json without a stop time is the
    //    take recording now, or one a crash cut off: the recovery card stamps
    //    its stop time, and the copy is tried again after that.
    if (! fs.exists (localFolder))
        return finish (result, Outcome::ComputerCopyMissing);

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

    if (! fs.exists (root))
    {
        const auto rootParent = parentOf (root);

        if (rootParent == root || ! fs.exists (rootParent))
            return finish (result, Outcome::CardMissing);

        if (const auto r = fs.makeFolder (root); ! r.ok())
            return refusedOnCard (outcomeForCardError (fs, rootParent, r), {}, r);
    }

    // 4. The take's folder on the card, remembered on this computer BEFORE it
    //    is made: a crash in between then reuses it, rather than leaving an
    //    empty folder behind and copying into "_2".
    auto cardFolder = marker.cardFolder;

    if (cardFolder.empty())
    {
        const auto name = SessionFolderNaming::resolveCollision (
            lastPartOf (localFolder),
            [&fs, &root] (const std::string& candidate) { return fs.exists (joinPath (root, candidate)); });

        cardFolder = joinPath (root, name);
    }

    result.cardFolder = cardFolder;

    if (cardFolder != marker.cardFolder)
        rememberOnThisComputer ({ root, cardFolder });

    if (! fs.exists (cardFolder))
        if (const auto r = fs.makeFolder (cardFolder); ! r.ok())
            return refusedOnCard (outcomeForCardError (fs, root, r), {}, r);

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
                    const auto outcome = outcomeForCardError (fs, root, wr);
                    abandon (writer);

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

    return finish (result, Outcome::Copied);
}

} // namespace

Result copyTake (CopyFileSystem& fs, const std::string& localFolder, const Marker& marker, Control& control)
{
    std::set<std::string> scratchVerified;
    auto& verified = control.verified != nullptr ? *control.verified : scratchVerified;

    auto result = copyTakeOnce (fs, localFolder, marker, control, verified);

    // A card that cannot be trusted with what it held a moment ago -- pulled,
    // swapped, garbled, or holding files that are not this take's -- has
    // every file of this take checked against it again next time. A full or
    // locked card, a file too big for its format, or a failure on this
    // computer's side says nothing about what is on it.
    const bool cardInDoubt = result.outcome == Outcome::CardMissing
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

std::string computerFullNote (const std::string& computerFolder, int takesStillToCopy)
{
    auto note = "This computer is nearly full, so this take is going straight to the card. To make room, "
                "delete takes in " + computerFolder + " that have already been copied to the card.";

    if (takesStillToCopy == 1)
        note += " 1 take there hasn't been copied yet. Keep it.";
    else if (takesStillToCopy > 1)
        note += " " + std::to_string (takesStillToCopy) + " takes there haven't been copied yet. Keep them.";

    return note;
}

bool isFailure (Outcome outcome) noexcept
{
    return outcome != Outcome::Copied && outcome != Outcome::Yielded && outcome != Outcome::Cancelled;
}

std::string stalledLine (const std::string& localFolder)
{
    return "The card stopped answering. This take is safe on this computer in " + localFolder + ".";
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

std::string describeWholeTake (const Result& result, const std::string& localFolder)
{
    const auto safe = " This take is safe on this computer in " + localFolder + ".";
    const auto kind = PreflightThroughputTest::filesystemKindFromTypeName (result.filesystemType);

    switch (result.outcome)
    {
        case Outcome::Copied:
            return "Copied to the card: " + result.cardFolder + ".";
        case Outcome::NotFinishedYet:
            return "This take didn't finish recording, so it hasn't been copied to the card yet."
                   + safe;
        case Outcome::ComputerCopyMissing:
            return "This take's folder on this computer is gone, so it couldn't be copied to the card.";
        case Outcome::ComputerReadFailed:
            return (result.fileName.empty() ? std::string ("A file") : result.fileName)
                   + " couldn't be read on this computer just now, so the copy to the card stopped. "
                     "It will be tried again." + safe;
        case Outcome::CardMissing:
            return "The card isn't plugged in. This take is safe on this computer in " + localFolder
                   + ", and will be copied when the card is back.";
        case Outcome::CardFull:
            return "The card is full." + safe;
        case Outcome::CardLocked:
            return PreflightThroughputTest::writeFailureReason (EROFS, kind) + safe;
        case Outcome::NotAllowed:
           #if defined(_WIN32)
            // Not the Mac's privacy settings: Windows refuses with "access
            // denied" for a read-only card and for Controlled folder access.
            return "SobStage isn't allowed to write to this card. Check the card isn't set to read-only, "
                   "and that Controlled folder access in Windows Security allows SobStage, or choose a "
                   "different place to save." + safe;
           #else
            return PreflightThroughputTest::writeFailureReason (EACCES, kind) + safe;
           #endif
        case Outcome::FileTooBig:
            return (result.fileName.empty() ? std::string ("A file") : result.fileName)
                   + " is too big for the way this card is formatted, so it wasn't copied. The rest of the "
                     "take was. To copy it, reformat the card as ExFAT (this erases it) or use another "
                     "card." + safe;
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

std::string describe (const Result& result, const std::string& localFolder)
{
    // The take itself already went: what is missing is only what was added
    // to its folder since. "Only on this computer" would be untrue of it.
    if (result.addedFilesOnly && isFailure (result.outcome))
    {
        if (result.outcome == Outcome::CardMissing)
            return "This take is on the card, but a file added to it since isn't yet: the card isn't "
                   "plugged in. It's safe on this computer in " + localFolder
                   + ", and will be copied when the card is back.";

        return "This take is on the card, but a file added to it since isn't yet. "
               + describeWholeTake (result, localFolder);
    }

    return describeWholeTake (result, localFolder);
}

} // namespace cardcopy
} // namespace mma
