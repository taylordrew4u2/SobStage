#include "TestFramework.h"
#include "Core/TakeCopy.h"
#include "Core/MirrorPolicy.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace mma;
using namespace mma::cardcopy;

// Record on this computer, then copy to the card. Every case names the
// mutation of TakeCopy.cpp it is there to catch. The one rule above all the
// others: the copy never changes the take on this computer, and nothing on
// the card has its real name until it has been read back and matched.

namespace {

constexpr const char* kLocal = "/home/u/RECORDINGS-MIRROR/2026-10-14_2000_Show";
constexpr const char* kRoot = "/cards/SD/RECORDINGS";
// Joined as the copy joins it: with '\' on Windows. Spelled out with '/', every
// case comparing a card path failed there (mma_takecopy_backslash_tests).
const std::string kCardTake = joinPath (kRoot, "2026-10-14_2000_Show");

constexpr const char* kFinishedRecord =
    "{ \"sessionName\": \"Show\", \"startTimestamp\": \"2026-10-14T20:00:00\", "
    "\"stopTimestamp\": \"2026-10-14T20:45:00\" }";
constexpr const char* kUnfinishedRecord =
    "{ \"sessionName\": \"Show\", \"startTimestamp\": \"2026-10-14T20:00:00\" }";

bool startsWith (const std::string& s, const std::string& prefix)
{
    return s.compare (0, prefix.size(), prefix) == 0;
}

std::string bytes (size_t n, unsigned seed)
{
    std::string s (n, '\0');
    unsigned x = seed * 2654435761u + 1u;
    for (auto& c : s)
    {
        x = x * 1103515245u + 12345u;
        c = static_cast<char> (x >> 16);
    }
    return s;
}

/// An in-memory disk. Every call is logged in order, with the cache flag on
/// reads, and any call can be made to fail.
class FakeFs : public CopyFileSystem
{
public:
    std::map<std::string, std::string> files;
    std::set<std::string> folders;

    std::optional<uint64_t> cardFree;
    bool cardIsFat32 = false;
    std::string cardType = "exfat";

    struct Failure
    {
        FsError error = FsError::Other;
        int osError = 5;
    };

    /// createNew fails for a path containing the key.
    std::map<std::string, Failure> failCreate;
    /// openRead fails for a path containing the key: a file busy elsewhere.
    std::map<std::string, Failure> failOpenRead;
    /// makeFolder fails for exactly this path.
    std::map<std::string, Failure> failMakeFolder;
    /// A write fails once the file whose name contains `writeFailPath` has
    /// had `writeFailAfterBytes` written to it.
    std::string writeFailPath;
    uint64_t writeFailAfterBytes = 0;
    Failure writeFailure;
    /// The card is pulled out when that write fails.
    bool pullCardOnWriteFailure = false;
    /// Read-backs of card files, past the cache, that come back corrupted.
    int corruptCardReads = 0;
    /// Renames not yet flushed by syncFolder, as a FAT card keeps them: a
    /// card pulled without ejecting comes back with the old names.
    std::vector<std::pair<std::string, std::string>> unflushedRenames;
    std::optional<Failure> failSyncFolder;
    /// A read hands back at most this much at a time.
    size_t readPiece = 1u << 20;

    struct Call
    {
        Call (std::string o, std::string p, std::string t = {}, bool bypass = false)
            : op (std::move (o)), path (std::move (p)), to (std::move (t)), bypassCache (bypass) {}

        std::string op, path, to;
        bool bypassCache = false;
    };
    std::vector<Call> calls;

    void addFolder (const std::string& path)
    {
        std::string p = path;
        while (! p.empty() && p != "/")
        {
            folders.insert (p);
            p = parentOf (p);
        }
        folders.insert ("/");
    }

    void addFile (const std::string& path, const std::string& content)
    {
        addFolder (parentOf (path));
        files[path] = content;
    }

    /// Pulled without ejecting, and put back: every rename not yet flushed
    /// is undone.
    void pullAndReinsertLosingUnflushedRenames()
    {
        for (auto it = unflushedRenames.rbegin(); it != unflushedRenames.rend(); ++it)
        {
            files[it->first] = files[it->second];
            files.erase (it->second);
        }
        unflushedRenames.clear();
    }

    void pullCard()
    {
        for (auto it = files.begin(); it != files.end();)
            it = startsWith (it->first, "/cards/SD") ? files.erase (it) : std::next (it);
        for (auto it = folders.begin(); it != folders.end();)
            it = startsWith (*it, "/cards/SD") ? folders.erase (it) : std::next (it);
    }

    int count (const std::string& op, const std::string& pathPart = {}) const
    {
        return static_cast<int> (std::count_if (calls.begin(), calls.end(), [&] (const Call& c)
        {
            return c.op == op && (pathPart.empty() || c.path.find (pathPart) != std::string::npos);
        }));
    }

    int callsUnder (const std::string& prefix) const
    {
        return static_cast<int> (std::count_if (calls.begin(), calls.end(), [&] (const Call& c)
        {
            return startsWith (c.path, prefix) || startsWith (c.to, prefix);
        }));
    }

    bool exists (const std::string& path) override
    {
        calls.push_back ({ "exists", path });
        return files.count (path) > 0 || folders.count (path) > 0;
    }

    FsResult list (const std::string& folder, std::vector<Entry>& entries) override
    {
        calls.push_back ({ "list", folder });
        entries.clear();
        if (folders.count (folder) == 0)
            return { FsError::NotFound, 2 };
        for (const auto& [path, content] : files)
            if (parentOf (path) == folder)
                entries.push_back ({ lastPartOf (path), content.size(), false });
        for (const auto& f : folders)
            if (f != folder && parentOf (f) == folder)
                entries.push_back ({ lastPartOf (f), 0, true });
        return {};
    }

    FsResult makeFolder (const std::string& path) override
    {
        calls.push_back ({ "makeFolder", path });
        if (const auto failing = failMakeFolder.find (path); failing != failMakeFolder.end())
            return { failing->second.error, failing->second.osError };
        if (folders.count (parentOf (path)) == 0)
            return { FsError::NotFound, 2 };
        if (folders.count (path) > 0 || files.count (path) > 0)
            return { FsError::Other, 17 };
        folders.insert (path);
        return {};
    }

    std::optional<uint64_t> freeBytes (const std::string& path) override
    {
        calls.push_back ({ "freeBytes", path });
        return startsWith (path, "/cards/") ? cardFree : std::optional<uint64_t> (1ull << 40);
    }

    std::optional<bool> limitedTo4GiBFiles (const std::string& path) override
    {
        calls.push_back ({ "limitedTo4GiBFiles", path });
        return startsWith (path, "/cards/") && cardIsFat32;
    }

    std::string filesystemTypeName (const std::string& path) override
    {
        calls.push_back ({ "filesystemTypeName", path });
        return cardType;
    }

    FsResult readText (const std::string& path, std::string& text) override
    {
        calls.push_back ({ "readText", path });
        const auto it = files.find (path);
        if (it == files.end())
            return { FsError::NotFound, 2 };
        text = it->second;
        return {};
    }

    FsResult replaceText (const std::string& path, const std::string& text) override
    {
        calls.push_back ({ "replaceText", path });
        if (folders.count (parentOf (path)) == 0)
            return { FsError::NotFound, 2 };
        files[path] = text;
        return {};
    }

    struct FakeReader : Reader
    {
        std::string data;
        size_t pos = 0;
        size_t piece = 0;

        FsResult read (void* buffer, size_t n, size_t& got) override
        {
            got = std::min ({ n, piece, data.size() - pos });
            std::copy (data.begin() + static_cast<long> (pos), data.begin() + static_cast<long> (pos + got),
                       static_cast<char*> (buffer));
            pos += got;
            return {};
        }
    };

    std::unique_ptr<Reader> openRead (const std::string& path, bool bypassCache, FsResult& result) override
    {
        calls.push_back ({ "openRead", path, {}, bypassCache });

        for (const auto& [key, failure] : failOpenRead)
            if (path.find (key) != std::string::npos)
            {
                result = { failure.error, failure.osError };
                return nullptr;
            }

        const auto it = files.find (path);
        if (it == files.end())
        {
            result = { FsError::NotFound, 2 };
            return nullptr;
        }

        auto reader = std::make_unique<FakeReader>();
        reader->data = it->second;
        reader->piece = readPiece;

        if (bypassCache && startsWith (path, "/cards/") && corruptCardReads > 0 && ! reader->data.empty())
        {
            --corruptCardReads;
            reader->data[reader->data.size() / 2] ^= 0x5a;
        }

        result = {};
        return reader;
    }

    struct FakeWriter : Writer
    {
        FakeFs* fs = nullptr;
        std::string path;
        uint64_t written = 0;

        FsResult write (const void* data, size_t n) override
        {
            fs->calls.push_back ({ "write", path });

            if (! fs->writeFailPath.empty() && path.find (fs->writeFailPath) != std::string::npos
                && written + n > fs->writeFailAfterBytes)
            {
                if (fs->pullCardOnWriteFailure)
                    fs->pullCard();
                return { fs->writeFailure.error, fs->writeFailure.osError };
            }

            if (fs->files.count (path) == 0)
                return { FsError::NotFound, 2 };

            fs->files[path].append (static_cast<const char*> (data), n);
            written += n;
            return {};
        }

        FsResult syncAndClose() override
        {
            fs->calls.push_back ({ "sync", path });
            return {};
        }
    };

    std::unique_ptr<Writer> createNew (const std::string& path, FsResult& result) override
    {
        calls.push_back ({ "createNew", path });

        for (const auto& [key, failure] : failCreate)
            if (path.find (key) != std::string::npos)
            {
                result = { failure.error, failure.osError };
                return nullptr;
            }

        if (files.count (path) > 0)
        {
            result = { FsError::Other, 17 };
            return nullptr;
        }

        if (folders.count (parentOf (path)) == 0)
        {
            result = { FsError::NotFound, 2 };
            return nullptr;
        }

        files[path] = {};
        auto writer = std::make_unique<FakeWriter>();
        writer->fs = this;
        writer->path = path;
        result = {};
        return writer;
    }

    FsResult rename (const std::string& from, const std::string& to, bool replace) override
    {
        calls.push_back ({ "rename", from, to });
        if (files.count (from) == 0)
            return { FsError::NotFound, 2 };
        if (! replace && files.count (to) > 0)
            return { FsError::Other, 17 };
        files[to] = files[from];
        files.erase (from);
        if (startsWith (to, "/cards/"))
            unflushedRenames.emplace_back (from, to);
        return {};
    }

    FsResult syncFolder (const std::string& path) override
    {
        calls.push_back ({ "syncFolder", path });
        if (failSyncFolder.has_value())
            return { failSyncFolder->error, failSyncFolder->osError };
        if (folders.count (path) == 0)
            return { FsError::NotFound, 2 };
        unflushedRenames.erase (std::remove_if (unflushedRenames.begin(), unflushedRenames.end(),
                                                [&path] (const auto& r) { return parentOf (r.second) == path; }),
                                unflushedRenames.end());
        return {};
    }

    FsResult remove (const std::string& path) override
    {
        calls.push_back ({ "remove", path });
        if (files.erase (path) == 0)
            return { FsError::NotFound, 2 };
        return {};
    }
};

std::string local (const std::string& name) { return joinPath (kLocal, name); }
std::string card (const std::string& name) { return joinPath (kCardTake, name); }

/// A finished take on this computer, and an empty card with its save folder.
void finishedTake (FakeFs& fs)
{
    fs.addFile (local ("MIX.wav"), bytes (300000, 1));
    fs.addFile (local ("01_Alice.wav"), bytes (200000, 2));
    fs.addFile (local ("02_Bob.wav"), bytes (200000, 3));
    fs.addFile (local ("activity.log"), "Recording started.\nRecording stopped.\n");
    fs.addFile (local ("session.json"), kFinishedRecord);
    fs.addFolder (kRoot);
}

Marker toTheCard()
{
    return { kRoot, {} };
}

Result run (FakeFs& fs, Marker marker = toTheCard(), Control* control = nullptr)
{
    Control fallback;
    return copyTake (fs, kLocal, marker, control != nullptr ? *control : fallback);
}

/// Every file on this computer but the marker, as it was.
std::map<std::string, std::string> computerCopy (const FakeFs& fs)
{
    std::map<std::string, std::string> out;
    for (const auto& [path, content] : fs.files)
        if (startsWith (path, kLocal) && lastPartOf (path) != kMarkerFileName)
            out[path] = content;
    return out;
}

bool noWorkingFilesOnCard (const FakeFs& fs)
{
    for (const auto& [path, content] : fs.files)
        if (startsWith (path, "/cards/") && isWorkingFileName (lastPartOf (path)))
            return false;
    return true;
}

} // namespace

// Mutation: a name changed on the way, or a file skipped.
TEST_CASE (TakeCopy_PathsKeepTheSeparatorTheFolderUses)
{
    // On every platform, so a Windows-only spelling is checked here too: a
    // path never mixes "/" and "\\", or a card folder no longer compares
    // equal to its own spelling.
    REQUIRE (joinPath ("/cards/SD/RECORDINGS", "Show") == "/cards/SD/RECORDINGS/Show");
    REQUIRE (joinPath ("C:\\Users\\me\\RECORDINGS-MIRROR", "Show")
             == "C:\\Users\\me\\RECORDINGS-MIRROR\\Show");
    REQUIRE (joinPath ("E:/RECORDINGS", "Show") == "E:/RECORDINGS/Show");
    REQUIRE (joinPath ("/cards/SD/", "Show") == "/cards/SD/Show");
}

TEST_CASE (TakeCopy_CopiesEveryFileWithTheSameNames)
{
    FakeFs fs;
    finishedTake (fs);

    const auto r = run (fs);
    REQUIRE (r.outcome == Outcome::Copied);
    REQUIRE (r.cardFolder == kCardTake);
    REQUIRE (r.filesCopied == 5);

    for (const char* name : { "MIX.wav", "01_Alice.wav", "02_Bob.wav", "activity.log", "session.json" })
    {
        REQUIRE (fs.files.count (card (name)) == 1);
        REQUIRE (fs.files[card (name)] == fs.files[local (name)]);
    }

    REQUIRE (noWorkingFilesOnCard (fs));
    REQUIRE (describe (r, kLocal) == std::string ("Copied to the card: ") + kCardTake + ".");
}

// Mutation: copyOrder reordered. A card folder holding session.json must hold
// everything else, because the card's recovery scan trusts that.
TEST_CASE (TakeCopy_SessionRecordGoesLast)
{
    FakeFs fs;
    finishedTake (fs);
    REQUIRE (run (fs).outcome == Outcome::Copied);

    std::vector<std::string> renamedTo;
    for (const auto& c : fs.calls)
        if (c.op == "rename")
            renamedTo.push_back (lastPartOf (c.to));

    REQUIRE (renamedTo.size() == 5u);
    REQUIRE (renamedTo[renamedTo.size() - 1] == "session.json");
    REQUIRE (renamedTo[renamedTo.size() - 2] == "activity.log");

    const auto order = copyOrder ({ "session.json", "MIX.wav", "activity.log", "01_A.wav", "V01_Cam.mov" });
    REQUIRE (order == (std::vector<std::string> { "01_A.wav", "MIX.wav", "V01_Cam.mov", "activity.log", "session.json" }));
}

// Mutation: the read-back removed, its answer ignored, or the computer's copy
// compared with itself.
TEST_CASE (TakeCopy_NothingTakesItsRealNameBeforeItIsVerified)
{
    FakeFs fs;
    finishedTake (fs);
    fs.corruptCardReads = 1000;

    const auto r = run (fs);
    REQUIRE (r.outcome == Outcome::CopyDidNotMatch);
    REQUIRE (r.fileName == "01_Alice.wav");

    for (const char* name : { "MIX.wav", "01_Alice.wav", "02_Bob.wav", "activity.log", "session.json" })
        REQUIRE (fs.files.count (card (name)) == 0);

    REQUIRE (noWorkingFilesOnCard (fs));
    REQUIRE (fs.count ("rename") == 0);
    REQUIRE (describe (r, kLocal).find ("didn't match") != std::string::npos);
}

// Mutation: the second try removed, or retried without end.
TEST_CASE (TakeCopy_TriesAMismatchOnceMore)
{
    FakeFs once;
    finishedTake (once);
    once.corruptCardReads = 1;

    REQUIRE (run (once).outcome == Outcome::Copied);
    REQUIRE (once.count ("createNew", "01_Alice.wav") == 2);
    REQUIRE (once.files[card ("01_Alice.wav")] == once.files[local ("01_Alice.wav")]);

    FakeFs always;
    finishedTake (always);
    always.corruptCardReads = 1000;

    REQUIRE (run (always).outcome == Outcome::CopyDidNotMatch);
    REQUIRE (always.count ("createNew", "01_Alice.wav") == 2);
}

// Mutation: the card read back through this computer's cache, which hands
// back what was written rather than what the card holds.
TEST_CASE (TakeCopy_ReadBackBypassesTheCache)
{
    FakeFs fs;
    finishedTake (fs);
    REQUIRE (run (fs).outcome == Outcome::Copied);

    int cardReads = 0;
    for (const auto& c : fs.calls)
    {
        if (c.op != "openRead")
            continue;

        if (startsWith (c.path, "/cards/"))
        {
            ++cardReads;
            REQUIRE (c.bypassCache);
        }
        else
        {
            REQUIRE_FALSE (c.bypassCache);
        }
    }

    REQUIRE (cardReads == 5);
}

// Mutation: the flush skipped, or done after the rename.
TEST_CASE (TakeCopy_FlushedBeforeRename)
{
    FakeFs fs;
    finishedTake (fs);
    REQUIRE (run (fs).outcome == Outcome::Copied);

    for (size_t i = 0; i < fs.calls.size(); ++i)
    {
        if (fs.calls[i].op != "rename")
            continue;

        const auto working = fs.calls[i].path;
        bool syncedBefore = false;
        for (size_t j = 0; j < i; ++j)
            syncedBefore = syncedBefore || (fs.calls[j].op == "sync" && fs.calls[j].path == working);

        REQUIRE (syncedBefore);
    }
}

// Mutation: the card's folder not flushed after a rename, or flushed only
// after "Copied". A card pulled without ejecting straight after must still
// have every file under its real name.
TEST_CASE (TakeCopy_RenamesAreOnTheCardBeforeItIsCopied)
{
    FakeFs fs;
    finishedTake (fs);
    fs.files[local (kMarkerFileName)] = markerToJson (toTheCard());

    REQUIRE (run (fs).outcome == Outcome::Copied);

    size_t lastRename = 0, lastSync = 0, markerGone = 0;
    for (size_t i = 0; i < fs.calls.size(); ++i)
    {
        const auto& c = fs.calls[i];
        if (c.op == "rename") lastRename = i;
        if (c.op == "syncFolder" && c.path == kCardTake) lastSync = i;
        if (c.op == "remove" && lastPartOf (c.path) == kMarkerFileName) markerGone = i;
    }
    REQUIRE (lastSync > lastRename);
    REQUIRE (markerGone > lastSync);

    fs.pullAndReinsertLosingUnflushedRenames();
    for (const char* name : { "MIX.wav", "01_Alice.wav", "02_Bob.wav", "activity.log", "session.json" })
        REQUIRE (fs.files.count (card (name)) == 1);

    // A card that cannot flush its folder is not called copied.
    FakeFs refusing;
    finishedTake (refusing);
    refusing.files[local (kMarkerFileName)] = markerToJson (toTheCard());
    refusing.failSyncFolder = FakeFs::Failure { FsError::Other, 5 };
    REQUIRE (run (refusing).outcome == Outcome::CardWriteFailed);
    REQUIRE (refusing.files.count (local (kMarkerFileName)) == 1);

    // A take found already on the card by a resumed copy is flushed too: the
    // attempt that renamed it may have been cut off before it could.
    FakeFs resumed;
    finishedTake (resumed);
    for (const char* name : { "MIX.wav", "01_Alice.wav", "02_Bob.wav", "activity.log", "session.json" })
        resumed.addFile (card (name), resumed.files[local (name)]);
    REQUIRE (run (resumed, { kRoot, kCardTake }).outcome == Outcome::Copied);
    REQUIRE (resumed.count ("syncFolder", kCardTake) >= 1);
}

// Mutation: a failed attempt keeping its take's checked files. The next one
// must look at the card again rather than trust it.
TEST_CASE (TakeCopy_AFailedAttemptForgetsWhatItChecked)
{
    FakeFs fs;
    finishedTake (fs);
    fs.writeFailPath = "MIX.wav";
    fs.writeFailure = { FsError::Other, 5 };

    Control control;
    std::set<std::string> verified;
    verified.insert (joinPath (joinPath (kRoot, "Another_Take"), "MIX.wav") + "|10");
    control.verified = &verified;

    REQUIRE (run (fs, toTheCard(), &control).outcome == Outcome::CardWriteFailed);
    REQUIRE (verified.size() == 1u); // only the other take's
}

// Mutation: any write, rename or remove under the take's folder on this
// computer, in any of the ways a copy can end.
TEST_CASE (TakeCopy_TheComputerCopyIsNeverChanged)
{
    struct Scenario
    {
        const char* name;
        void (*setup) (FakeFs&);
        Outcome expected;
    };

    const Scenario scenarios[] = {
        { "copied", [] (FakeFs&) {}, Outcome::Copied },
        { "card missing", [] (FakeFs& fs) { fs.pullCard(); }, Outcome::CardMissing },
        { "card full", [] (FakeFs& fs) { fs.cardFree = 1000; }, Outcome::CardFull },
        { "locked", [] (FakeFs& fs) { fs.failCreate[".sobcopy"] = { FsError::ReadOnly, 30 }; }, Outcome::CardLocked },
        { "not allowed", [] (FakeFs& fs) { fs.failCreate[".sobcopy"] = { FsError::NotAllowed, 1 }; }, Outcome::NotAllowed },
        { "too big", [] (FakeFs& fs) { fs.writeFailPath = "MIX"; fs.writeFailure = { FsError::TooBig, 27 }; }, Outcome::FileTooBig },
        { "write failed", [] (FakeFs& fs) { fs.writeFailPath = "MIX"; fs.writeFailure = { FsError::Other, 5 }; }, Outcome::CardWriteFailed },
        { "pulled mid-copy", [] (FakeFs& fs) { fs.writeFailPath = "MIX"; fs.pullCardOnWriteFailure = true; }, Outcome::CardMissing },
        { "did not match", [] (FakeFs& fs) { fs.corruptCardReads = 1000; }, Outcome::CopyDidNotMatch },
        { "name taken", [] (FakeFs& fs) { fs.addFile (card ("MIX.wav"), "someone else's take"); }, Outcome::NameTakenOnCard },
    };

    for (const auto& s : scenarios)
    {
        FakeFs fs;
        finishedTake (fs);
        s.setup (fs);

        // Its card folder already chosen, so a file already on the card is
        // in the way rather than a reason to pick another folder.
        const auto before = computerCopy (fs);
        const auto r = run (fs, { kRoot, kCardTake });

        if (r.outcome != s.expected)
            mmatest::failAt (__FILE__, __LINE__, std::string ("unexpected outcome for ") + s.name);

        REQUIRE (computerCopy (fs) == before);

        for (const auto& c : fs.calls)
        {
            const bool touchesComputer = startsWith (c.path, kLocal) || startsWith (c.to, kLocal);
            const bool changes = c.op == "write" || c.op == "createNew" || c.op == "rename"
                              || c.op == "remove" || c.op == "makeFolder" || c.op == "replaceText";

            if (touchesComputer && changes && lastPartOf (c.path) != kMarkerFileName)
                mmatest::failAt (__FILE__, __LINE__, std::string (s.name) + ": changed " + c.path);
        }

        // Every failure says where the take is safe.
        if (isFailure (r.outcome))
            REQUIRE (describe (r, kLocal).find (kLocal) != std::string::npos);
    }
}

// Mutation: the stop-time check removed. The take being recorded right now has
// a session.json without one, and must not be copied while it grows.
TEST_CASE (TakeCopy_AnUnfinishedTakeIsNotCopied)
{
    FakeFs fs;
    finishedTake (fs);
    fs.files[local ("session.json")] = kUnfinishedRecord;

    const auto r = run (fs);
    REQUIRE (r.outcome == Outcome::NotFinishedYet);
    REQUIRE (fs.callsUnder ("/cards/") == 0);

    // No record at all is the same: not finished yet.
    FakeFs none;
    finishedTake (none);
    none.files.erase (local ("session.json"));
    REQUIRE (run (none).outcome == Outcome::NotFinishedYet);
    REQUIRE (none.callsUnder ("/cards/") == 0);
}

// Mutation: a recursive mkdir, or the parent's existence check removed. A card
// that is not plugged in must not have its folders made on this computer.
TEST_CASE (TakeCopy_MissingCardCreatesNothing)
{
    FakeFs fs;
    finishedTake (fs);
    fs.pullCard();

    const auto r = run (fs);
    REQUIRE (r.outcome == Outcome::CardMissing);
    REQUIRE (fs.count ("makeFolder") == 0);
    REQUIRE (fs.count ("createNew") == 0);
    REQUIRE (describe (r, kLocal).find ("isn't plugged in") != std::string::npos);
    REQUIRE (describe (r, kLocal).find ("will be copied when the card is back") != std::string::npos);

    // A Mac's card at /Volumes/SD, out: /Volumes is there, on the startup
    // disk, and the card's own folder must never be made inside it.
    FakeFs mac;
    finishedTake (mac);
    mac.addFolder ("/Volumes");
    REQUIRE (run (mac, { "/Volumes/SD", {} }).outcome == Outcome::CardMissing);
    REQUIRE (run (mac, { "/Volumes/SD/RECORDINGS", {} }).outcome == Outcome::CardMissing);
    REQUIRE (mac.count ("makeFolder") == 0);
}

TEST_CASE (TakeCopy_TheSaveFolderIsMadeOneLevelOnACardThatIsThere)
{
    FakeFs fs;
    finishedTake (fs);
    fs.folders.erase (kRoot); // the card is in, its RECORDINGS folder is not

    REQUIRE (run (fs).outcome == Outcome::Copied);
    REQUIRE (fs.count ("makeFolder", kRoot) == 2); // RECORDINGS, then the take
    REQUIRE (fs.files.count (card ("MIX.wav")) == 1);
}

// Mutation: the room check removed, or < and <= swapped.
TEST_CASE (TakeCopy_FullCardIsRefusedBeforeWriting)
{
    uint64_t total = 0;
    {
        FakeFs sizes;
        finishedTake (sizes);
        for (const auto& [path, content] : sizes.files)
            if (startsWith (path, kLocal))
                total += content.size();
    }

    FakeFs shortOne;
    finishedTake (shortOne);
    shortOne.cardFree = total + kCardMarginBytes - 1;
    const auto r = run (shortOne);
    REQUIRE (r.outcome == Outcome::CardFull);
    REQUIRE (shortOne.count ("createNew") == 0);
    REQUIRE (describe (r, kLocal).find ("The card is full.") == 0);

    FakeFs exactly;
    finishedTake (exactly);
    exactly.cardFree = total + kCardMarginBytes;
    REQUIRE (run (exactly).outcome == Outcome::Copied);
}

// Mutation: the error mapping, or the clean-up after a failed write.
TEST_CASE (TakeCopy_CardFillingUpMidCopy)
{
    FakeFs fs;
    finishedTake (fs);
    fs.writeFailPath = "MIX.wav";
    fs.writeFailAfterBytes = 100;
    fs.writeFailure = { FsError::NoSpace, 28 };

    const auto r = run (fs);
    REQUIRE (r.outcome == Outcome::CardFull);
    REQUIRE (r.fileName == "MIX.wav");
    REQUIRE (noWorkingFilesOnCard (fs));
    REQUIRE (fs.files.count (card ("MIX.wav")) == 0);
}

// Mutation: ReadOnly and NotAllowed merged. A lock switch and a macOS privacy
// refusal have different fixes.
TEST_CASE (TakeCopy_LockedCardAndRefusedPermissionAreToldApart)
{
    FakeFs locked;
    finishedTake (locked);
    locked.failCreate[".sobcopy"] = { FsError::ReadOnly, 30 };
    const auto l = run (locked);
    REQUIRE (l.outcome == Outcome::CardLocked);
    REQUIRE (describe (l, kLocal).find ("lock switch") != std::string::npos);

    FakeFs refused;
    finishedTake (refused);
    refused.failCreate[".sobcopy"] = { FsError::NotAllowed, 1 };
    const auto n = run (refused);
    REQUIRE (n.outcome == Outcome::NotAllowed);
    REQUIRE (describe (n, kLocal).find ("isn't allowed") != std::string::npos);
    REQUIRE (describe (n, kLocal).find ("lock switch") == std::string::npos);

    // A Windows-formatted drive a Mac mounts read-only has no lock switch.
    FakeFs ntfs;
    finishedTake (ntfs);
    ntfs.cardType = "ntfs";
    ntfs.failCreate[".sobcopy"] = { FsError::ReadOnly, 30 };
    const auto w = run (ntfs);
    REQUIRE (w.outcome == Outcome::CardLocked);
    REQUIRE (describe (w, kLocal, Wording::Mac).find ("NTFS") != std::string::npos);

    // Mutation: the folder's refusal not asked the card's format. The copy's
    // first write to such a drive is always a folder, and it must say NTFS
    // there too -- for the take's folder and for the save folder itself.
    FakeFs ntfsFolder;
    finishedTake (ntfsFolder);
    ntfsFolder.cardType = "ntfs";
    ntfsFolder.failMakeFolder[kCardTake] = { FsError::ReadOnly, 30 };
    const auto f = run (ntfsFolder);
    REQUIRE (f.outcome == Outcome::CardLocked);
    REQUIRE (describe (f, kLocal, Wording::Mac).find ("NTFS") != std::string::npos);
    REQUIRE (describe (f, kLocal, Wording::Mac).find ("lock switch") == std::string::npos);

    FakeFs ntfsRoot;
    finishedTake (ntfsRoot);
    ntfsRoot.cardType = "ntfs";
    ntfsRoot.folders.erase (kRoot);
    ntfsRoot.failMakeFolder[kRoot] = { FsError::ReadOnly, 30 };
    const auto rr = run (ntfsRoot);
    REQUIRE (rr.outcome == Outcome::CardLocked);
    REQUIRE (describe (rr, kLocal, Wording::Mac).find ("NTFS") != std::string::npos);

    // Windows refuses a read-only card or Controlled folder access with
    // "access denied"; it is not sent to a Mac's System Settings.
    REQUIRE (describe (n, kLocal, Wording::Windows).find ("System Settings") == std::string::npos);
    REQUIRE (describe (n, kLocal, Wording::Windows).find ("Windows Security") != std::string::npos);
    REQUIRE (describe (n, kLocal, Wording::Mac).find ("Privacy & Security") != std::string::npos);
}

// Mutation: the NTFS wording given on Windows. Windows writes NTFS; a drive it
// will not write is write-protected -- a lock switch, or a policy such as
// BitLocker's on removable drives -- and the Mac's "reformat it in Disk
// Utility (this erases it)" sent a Windows user to erase a drive that may
// hold earlier takes instead of turning the protection off.
TEST_CASE (TakeCopy_AWriteProtectedDriveOnWindowsIsNotCalledAMacProblem)
{
    FakeFs ntfs;
    finishedTake (ntfs);
    ntfs.cardType = "NTFS";
    ntfs.failCreate[".sobcopy"] = { FsError::ReadOnly, 19 }; // ERROR_WRITE_PROTECT
    const auto r = run (ntfs);
    REQUIRE (r.outcome == Outcome::CardLocked);

    const auto onWindows = describe (r, kLocal, Wording::Windows);
    REQUIRE (onWindows.find ("macOS") == std::string::npos);
    REQUIRE (onWindows.find ("Disk Utility") == std::string::npos);
    REQUIRE (onWindows.find ("reformat") == std::string::npos);
    REQUIRE (onWindows.find ("write-protected") != std::string::npos);
    REQUIRE (onWindows.find ("BitLocker") != std::string::npos);
    REQUIRE (onWindows.find ("safe on this computer in " + std::string (kLocal)) != std::string::npos);

    // Whatever its format.
    FakeFs exfat;
    finishedTake (exfat);
    exfat.failCreate[".sobcopy"] = { FsError::ReadOnly, 19 };
    REQUIRE (describe (run (exfat), kLocal, Wording::Windows).find ("write-protected") != std::string::npos);

    // A Mac still hears what it can do about a Windows-formatted drive.
    REQUIRE (describe (r, kLocal, Wording::Mac).find ("NTFS") != std::string::npos);
}

// Mutation: the 4 GiB check removed, at either point.
TEST_CASE (TakeCopy_FileTooBigForTheCard)
{
    // Checked before anything is written, from the size alone: the listing
    // says 4 GiB, which no test needs to hold in memory.
    struct BigListing : FakeFs
    {
        FsResult list (const std::string& folder, std::vector<Entry>& entries) override
        {
            const auto r = FakeFs::list (folder, entries);
            for (auto& e : entries)
                if (e.name == "V01_Cam.mov")
                    e.size = kFat32FileLimitBytes;
            return r;
        }
    };

    BigListing big;
    finishedTake (big);
    big.cardIsFat32 = true;
    big.addFile (local ("V01_Cam.mov"), "x");
    big.files[local (kMarkerFileName)] = markerToJson (toTheCard());

    const auto r = run (big);
    REQUIRE (r.outcome == Outcome::FileTooBig);
    REQUIRE (r.fileName == "V01_Cam.mov");
    REQUIRE (big.count ("createNew", "V01_Cam.mov") == 0);
    REQUIRE (describe (r, kLocal).find ("V01_Cam.mov is too big") == 0);
    REQUIRE (describe (r, kLocal).find ("reformat the card as ExFAT") != std::string::npos);

    // The sound is not held back by a movie that can never fit: everything
    // else is on the card, but not session.json, so the card's folder never
    // looks complete without the movie. The take stays marked to be copied.
    for (const char* name : { "MIX.wav", "01_Alice.wav", "02_Bob.wav", "activity.log" })
        REQUIRE (big.files.count (card (name)) == 1 && big.files[card (name)] == big.files[local (name)]);
    REQUIRE (big.files.count (card ("V01_Cam.mov")) == 0);
    REQUIRE (big.files.count (card ("session.json")) == 0);
    REQUIRE (big.files.count (local (kMarkerFileName)) == 1);
    REQUIRE (noWorkingFilesOnCard (big));

    // One byte under the limit is fine.
    struct JustUnder : BigListing
    {
        FsResult list (const std::string& folder, std::vector<Entry>& entries) override
        {
            const auto r2 = FakeFs::list (folder, entries);
            for (auto& e : entries)
                if (e.name == "V01_Cam.mov")
                    e.size = kFat32FileLimitBytes - 1;
            return r2;
        }
    };

    JustUnder under;
    finishedTake (under);
    under.cardIsFat32 = true;
    under.addFile (local ("V01_Cam.mov"), "x");
    REQUIRE (run (under).outcome != Outcome::FileTooBig);

    // And when the card says so at write time: the rest still goes.
    FakeFs late;
    finishedTake (late);
    late.writeFailPath = "MIX.wav";
    late.writeFailure = { FsError::TooBig, 27 };
    const auto t = run (late);
    REQUIRE (t.outcome == Outcome::FileTooBig);
    REQUIRE (t.fileName == "MIX.wav");
    REQUIRE (noWorkingFilesOnCard (late));
    REQUIRE (late.files.count (card ("MIX.wav")) == 0);
    REQUIRE (late.files.count (card ("02_Bob.wav")) == 1);
    REQUIRE (late.files.count (card ("activity.log")) == 1);
    REQUIRE (late.files.count (card ("session.json")) == 0);
}

// Mutation: replace allowed for audio. A different MIX.wav already on the card
// is a recording, and is never written over.
TEST_CASE (TakeCopy_NeverOverwritesAudioOnTheCard)
{
    // Same size, different bytes: only a read can tell.
    FakeFs fs;
    finishedTake (fs);
    auto other = fs.files[local ("MIX.wav")];
    other[10] ^= 1;
    fs.addFile (card ("MIX.wav"), other);

    // A resumed copy into the folder it chose before.
    const auto r = run (fs, { kRoot, kCardTake });
    REQUIRE (r.outcome == Outcome::NameTakenOnCard);
    REQUIRE (r.fileName == "MIX.wav");
    REQUIRE (fs.files[card ("MIX.wav")] == other);
    REQUIRE (describe (r, kLocal).find ("already has a different file called MIX.wav") != std::string::npos);

    for (const auto& c : fs.calls)
        REQUIRE_FALSE (c.op == "rename" && c.to == card ("MIX.wav"));

    // Different size: refused before anything is written.
    FakeFs sized;
    finishedTake (sized);
    sized.addFile (card ("MIX.wav"), "short");
    REQUIRE (run (sized, { kRoot, kCardTake }).outcome == Outcome::NameTakenOnCard);
    REQUIRE (sized.count ("createNew") == 0);
    REQUIRE (sized.files[card ("MIX.wav")] == "short");
}

// Mutation: the record files refused like audio. session.json and activity.log
// are records of the take, and the card's should say what the computer's says.
TEST_CASE (TakeCopy_RefreshesTheTakeRecordOnTheCard)
{
    FakeFs fs;
    finishedTake (fs);
    fs.addFile (card ("session.json"), "{ \"old\": true }");

    REQUIRE (run (fs, { kRoot, kCardTake }).outcome == Outcome::Copied);
    REQUIRE (fs.files[card ("session.json")] == kFinishedRecord);
}

// Mutation: every file always rewritten. After a quit, what is already on the
// card and matches is checked and kept.
TEST_CASE (TakeCopy_ResumesWithoutRecopyingVerifiedFiles)
{
    FakeFs fs;
    finishedTake (fs);
    fs.addFile (card ("01_Alice.wav"), fs.files[local ("01_Alice.wav")]);
    fs.addFile (card ("02_Bob.wav"), fs.files[local ("02_Bob.wav")]);

    const auto r = run (fs, { kRoot, kCardTake });
    REQUIRE (r.outcome == Outcome::Copied);
    REQUIRE (fs.count ("createNew", "01_Alice") == 0);
    REQUIRE (fs.count ("createNew", "02_Bob") == 0);
    REQUIRE (fs.count ("createNew", "MIX.wav") == 1);
    REQUIRE (r.filesCopied == 3);
}

// Mutation: a half-written copy from an attempt that was cut off left behind.
TEST_CASE (TakeCopy_ClearsItsOwnLeftoversOnResume)
{
    FakeFs fs;
    finishedTake (fs);
    fs.addFile (card (".MIX.wav.sobcopy"), "half a file");
    fs.addFile (card (".01_Alice.wav.sobcopy"), "and another");

    REQUIRE (run (fs, { kRoot, kCardTake }).outcome == Outcome::Copied);
    REQUIRE (noWorkingFilesOnCard (fs));
    REQUIRE (fs.files[card ("MIX.wav")] == fs.files[local ("MIX.wav")]);
}

// Mutation: shouldYield ignored, or asked only once per file. A take started
// mid-copy must have the disk to itself within one chunk.
TEST_CASE (TakeCopy_YieldsBetweenChunksAndLeavesNoPartialFile)
{
    FakeFs fs;
    finishedTake (fs);
    fs.addFile (local ("V01_Cam.mov"), bytes (3 * kChunkBytes, 9));

    Control control;
    std::set<std::string> verified;
    control.verified = &verified;
    control.shouldYield = [&fs] { return fs.count ("write", "V01_Cam.mov") >= 1; };

    const auto r = run (fs, toTheCard(), &control);
    REQUIRE (r.outcome == Outcome::Yielded);
    REQUIRE (fs.count ("write", "V01_Cam.mov") == 1);
    REQUIRE (noWorkingFilesOnCard (fs));
    REQUIRE (fs.files.count (card ("V01_Cam.mov")) == 0);
    REQUIRE (describe (r, kLocal).empty());

    // Let go: it finishes, without writing again what was already checked.
    control.shouldYield = nullptr;
    const auto again = run (fs, { kRoot, r.cardFolder }, &control);
    REQUIRE (again.outcome == Outcome::Copied);
    REQUIRE (fs.files[card ("V01_Cam.mov")] == fs.files[local ("V01_Cam.mov")]);
    REQUIRE (fs.count ("createNew", "01_Alice") == 1);
}

// Mutation: the cancel check removed.
TEST_CASE (TakeCopy_CancelLeavesNoPartialFile)
{
    FakeFs fs;
    finishedTake (fs);
    fs.addFile (local ("V01_Cam.mov"), bytes (3 * kChunkBytes, 9));

    std::atomic<bool> cancel { false };
    Control control;
    control.cancel = &cancel;
    control.progress = [&fs, &cancel] (uint64_t, uint64_t)
    {
        if (fs.count ("write", "V01_Cam.mov") >= 1)
            cancel.store (true);
    };

    const auto r = run (fs, toTheCard(), &control);
    REQUIRE (r.outcome == Outcome::Cancelled);
    REQUIRE (fs.count ("write", "V01_Cam.mov") == 1);
    REQUIRE (noWorkingFilesOnCard (fs));
    REQUIRE (fs.files.count (card ("V01_Cam.mov")) == 0);
    // The marker stays, so the copy is picked up at the next launch.
    REQUIRE (fs.files.count (local (kMarkerFileName)) == 1);
}

// Mutation: shouldCopy wrong.
TEST_CASE (TakeCopy_SkipsWorkingHiddenAndMarkerFiles)
{
    REQUIRE_FALSE (shouldCopy ("MIX - Apple Podcasts.wav.part"));
    REQUIRE_FALSE (shouldCopy (".V01_Cam_with-sound.mov"));
    REQUIRE_FALSE (shouldCopy ("._MIX.wav"));
    REQUIRE_FALSE (shouldCopy (".DS_Store"));
    REQUIRE_FALSE (shouldCopy (kMarkerFileName));
    REQUIRE_FALSE (shouldCopy (".MIX.wav.sobcopy"));
    REQUIRE (shouldCopy ("MIX.wav"));
    REQUIRE (shouldCopy ("V01_Cam_with-sound.mov"));
    REQUIRE (shouldCopy ("MIX - Apple Podcasts.wav"));
    REQUIRE (shouldCopy ("session.json"));

    FakeFs fs;
    finishedTake (fs);
    fs.addFile (local ("MIX - Apple Podcasts.wav.part"), "half");
    fs.addFile (local ("._MIX.wav"), "appledouble");
    fs.addFile (local (".DS_Store"), "finder");
    fs.addFile (local (kMarkerFileName), markerToJson (toTheCard()));

    REQUIRE (run (fs).outcome == Outcome::Copied);
    REQUIRE (fs.files.count (card ("MIX - Apple Podcasts.wav.part")) == 0);
    REQUIRE (fs.files.count (card ("._MIX.wav")) == 0);
    REQUIRE (fs.files.count (card (".DS_Store")) == 0);
    REQUIRE (fs.files.count (card (kMarkerFileName)) == 0);
}

// Mutation: the marker written after the folder is made, or its folder
// ignored. A clash takes the next free name, and a second run uses it again.
TEST_CASE (TakeCopy_NameClashTakesNextFreeNameAndRemembersIt)
{
    FakeFs fs;
    finishedTake (fs);
    fs.addFolder (kCardTake); // another take of the same name, already there
    fs.writeFailPath = "MIX.wav";
    fs.writeFailure = { FsError::Other, 5 };

    const auto first = run (fs);
    REQUIRE (first.outcome == Outcome::CardWriteFailed);
    REQUIRE (first.cardFolder == std::string (kCardTake) + "_2");

    // Remembered on this computer before the folder was made.
    size_t markerAt = fs.calls.size(), folderAt = fs.calls.size();
    for (size_t i = 0; i < fs.calls.size(); ++i)
    {
        if (fs.calls[i].op == "replaceText" && lastPartOf (fs.calls[i].path) == kMarkerFileName && markerAt == fs.calls.size())
            markerAt = i;
        if (fs.calls[i].op == "makeFolder" && fs.calls[i].path == first.cardFolder)
            folderAt = i;
    }
    REQUIRE (markerAt < folderAt);

    const auto remembered = markerFromJson (fs.files[local (kMarkerFileName)]);
    REQUIRE (remembered.has_value());
    REQUIRE (remembered->cardFolder == first.cardFolder);

    // The next run, from the marker alone, goes back to _2 and makes no _3.
    fs.writeFailPath.clear();
    const auto second = run (fs, *remembered);
    REQUIRE (second.outcome == Outcome::Copied);
    REQUIRE (second.cardFolder == first.cardFolder);
    REQUIRE (fs.folders.count (std::string (kCardTake) + "_3") == 0);
    REQUIRE (fs.files.count (joinPath (first.cardFolder, "MIX.wav")) == 1);
}

// Mutation: the marker removed before the last file is on the card.
TEST_CASE (TakeCopy_MarkerRemovedOnlyWhenEverythingIsOnTheCard)
{
    FakeFs fs;
    finishedTake (fs);
    fs.files[local (kMarkerFileName)] = markerToJson (toTheCard());
    fs.writeFailPath = "session.json";
    fs.writeFailure = { FsError::Other, 5 };

    REQUIRE (run (fs).outcome == Outcome::CardWriteFailed);
    REQUIRE (fs.files.count (local (kMarkerFileName)) == 1);

    fs.writeFailPath.clear();
    REQUIRE (run (fs, *markerFromJson (fs.files[local (kMarkerFileName)])).outcome == Outcome::Copied);
    REQUIRE (fs.files.count (local (kMarkerFileName)) == 0);
}

// Mutation: the verified set ignored. A re-run with nothing new must not
// touch the card at all.
TEST_CASE (TakeCopy_AlreadyVerifiedFilesDoNotTouchTheCard)
{
    FakeFs fs;
    finishedTake (fs);

    Control control;
    std::set<std::string> verified;
    control.verified = &verified;

    const auto first = run (fs, toTheCard(), &control);
    REQUIRE (first.outcome == Outcome::Copied);
    REQUIRE (verified.size() == 5u);

    fs.calls.clear();
    const auto again = run (fs, { kRoot, first.cardFolder }, &control);
    REQUIRE (again.outcome == Outcome::Copied);
    REQUIRE (fs.callsUnder ("/cards/") == 0);

    // Something new arrives -- a combined video -- and only it is written.
    fs.addFile (local ("V01_Cam_with-sound.mov"), bytes (5000, 7));
    fs.calls.clear();
    const auto resync = run (fs, { kRoot, first.cardFolder }, &control);
    REQUIRE (resync.outcome == Outcome::Copied);
    REQUIRE (fs.count ("createNew") == 1);
    REQUIRE (fs.count ("openRead", "/cards/") == 1);
    REQUIRE (fs.files[card ("V01_Cam_with-sound.mov")] == fs.files[local ("V01_Cam_with-sound.mov")]);
}

// Mutation: a file checked earlier in the run skipped without looking at the
// card now. The card reader drops out after the first files are checked and
// comes back empty -- the same card with its last folder updates lost, or
// another card with the same name. Every file must reach the card again.
TEST_CASE (TakeCopy_ACardThatCameBackIsCheckedNotTrusted)
{
    FakeFs fs;
    finishedTake (fs);
    fs.writeFailPath = "MIX.wav";
    fs.pullCardOnWriteFailure = true;

    Control control;
    std::set<std::string> verified;
    control.verified = &verified;

    const auto first = run (fs, toTheCard(), &control);
    REQUIRE (first.outcome == Outcome::CardMissing);

    fs.writeFailPath.clear();
    fs.addFolder (kRoot);

    const auto again = run (fs, { kRoot, first.cardFolder }, &control);
    REQUIRE (again.outcome == Outcome::Copied);

    for (const char* name : { "MIX.wav", "01_Alice.wav", "02_Bob.wav", "activity.log", "session.json" })
        REQUIRE (fs.files.count (card (name)) == 1 && fs.files[card (name)] == fs.files[local (name)]);
}

// Mutation: the same, for a card swapped while the take's copy was copied and
// a combined video arrived after. The new card gets the whole take, not only
// the new file.
TEST_CASE (TakeCopy_ASwappedCardGetsTheWholeTake)
{
    FakeFs fs;
    finishedTake (fs);

    Control control;
    std::set<std::string> verified;
    control.verified = &verified;

    const auto first = run (fs, toTheCard(), &control);
    REQUIRE (first.outcome == Outcome::Copied);

    fs.pullCard();
    fs.addFolder (kRoot); // another card, same name, nothing on it

    fs.addFile (local ("V01_Cam_with-sound.mov"), bytes (5000, 7));
    const auto again = run (fs, { kRoot, first.cardFolder }, &control);
    REQUIRE (again.outcome == Outcome::Copied);

    for (const char* name : { "MIX.wav", "01_Alice.wav", "02_Bob.wav", "V01_Cam_with-sound.mov",
                              "activity.log", "session.json" })
        REQUIRE (fs.files.count (card (name)) == 1 && fs.files[card (name)] == fs.files[local (name)]);

    // A file still on the card under its name and size is not read again.
    fs.addFile (local ("MIX - Apple Podcasts.wav"), bytes (4000, 8));
    fs.calls.clear();
    REQUIRE (run (fs, { kRoot, first.cardFolder }, &control).outcome == Outcome::Copied);
    REQUIRE (fs.count ("openRead", "/cards/") == 1);
}

// Mutation: the card's root not looked at again after a failed write. A card
// pulled mid-copy answers with any error at all.
TEST_CASE (TakeCopy_CardPulledMidCopyIsCalledMissing)
{
    FakeFs fs;
    finishedTake (fs);
    fs.writeFailPath = "MIX.wav";
    fs.writeFailure = { FsError::Other, 5 };
    fs.pullCardOnWriteFailure = true;

    const auto r = run (fs);
    REQUIRE (r.outcome == Outcome::CardMissing);
}

// Mutation: keepMarker ignored. While a combined video or podcast copy is
// still being made, a finished copy must leave the note for the next launch.
TEST_CASE (TakeCopy_TheMarkerStaysWhileMoreFilesAreComing)
{
    FakeFs fs;
    finishedTake (fs);
    fs.files[local (kMarkerFileName)] = markerToJson (toTheCard());

    Control control;
    std::set<std::string> verified;
    control.verified = &verified;
    control.keepMarker = true;

    const auto first = run (fs, toTheCard(), &control);
    REQUIRE (first.outcome == Outcome::Copied);
    REQUIRE (fs.files.count (local (kMarkerFileName)) == 1);
    REQUIRE (markerFromJson (fs.files[local (kMarkerFileName)])->cardFolder == kCardTake);

    // The last file arrives and goes; then the note goes.
    fs.addFile (local ("V01_Cam_with-sound.mov"), bytes (5000, 7));
    control.keepMarker = false;
    REQUIRE (run (fs, { kRoot, kCardTake }, &control).outcome == Outcome::Copied);
    REQUIRE (fs.files.count (local (kMarkerFileName)) == 0);
}

// Mutation: the note written back only after the card is found. A file added
// after the copy finished, with the card out by then, must still be copied at
// the next launch.
TEST_CASE (TakeCopy_ARecopyRemembersTheTakeBeforeAskingTheCard)
{
    FakeFs fs;
    finishedTake (fs);
    fs.pullCard();
    REQUIRE (fs.files.count (local (kMarkerFileName)) == 0); // removed by the first copy

    const auto r = run (fs, { kRoot, kCardTake });
    REQUIRE (r.outcome == Outcome::CardMissing);
    REQUIRE (fs.files.count (local (kMarkerFileName)) == 1);

    const auto remembered = markerFromJson (fs.files[local (kMarkerFileName)]);
    REQUIRE (remembered.has_value());
    REQUIRE (remembered->destinationRoot == kRoot);
    REQUIRE (remembered->cardFolder == kCardTake);
}

// Mutation: every local read failure called "the folder is gone". A file busy
// for a moment -- a combined video still being written, antivirus -- is tried
// again; only a folder that is really gone is final.
TEST_CASE (TakeCopy_AFileBusyOnThisComputerIsNotAFolderGone)
{
    FakeFs fs;
    finishedTake (fs);
    fs.failOpenRead[local ("MIX.wav")] = { FsError::Other, 32 };

    Control control;
    std::set<std::string> verified;
    control.verified = &verified;

    const auto r = run (fs, toTheCard(), &control);
    REQUIRE (r.outcome == Outcome::ComputerReadFailed);
    REQUIRE (r.fileName == "MIX.wav");
    REQUIRE (isFailure (r.outcome));
    REQUIRE (describe (r, kLocal).find ("MIX.wav couldn't be read") == 0);
    REQUIRE (describe (r, kLocal).find ("tried again") != std::string::npos);
    // Nothing about the card is in doubt: what it checked is kept.
    REQUIRE (verified.size() == 2u);

    fs.failOpenRead.clear();
    REQUIRE (run (fs, { kRoot, r.cardFolder }, &control).outcome == Outcome::Copied);

    // The folder itself gone mid-copy -- renamed or deleted -- is said as
    // that: the caller follows a rename, and nothing else is worth retrying.
    struct FolderMovesAway : FakeFs
    {
        std::unique_ptr<Reader> openRead (const std::string& path, bool bypassCache, FsResult& result) override
        {
            if (path == local ("MIX.wav"))
            {
                folders.erase (kLocal);
                result = { FsError::NotFound, 2 };
                return nullptr;
            }
            return FakeFs::openRead (path, bypassCache, result);
        }
    };

    FolderMovesAway moved;
    finishedTake (moved);
    REQUIRE (run (moved).outcome == Outcome::ComputerCopyMissing);
}

// Mutation: a re-copy's failure worded as the whole take being only on this
// computer, when the take is on the card and only the file added since is not.
TEST_CASE (TakeCopyWording_AFileAddedLaterIsNotTheWholeTake)
{
    const std::string where = "/Users/sam/RECORDINGS-MIRROR/2026-10-14_2000_Show";

    Result r;
    r.outcome = Outcome::CardMissing;
    r.addedFilesOnly = true;
    const auto line = describe (r, where);
    REQUIRE (line.find ("This take is on the card, but a file added to it since isn't yet") == 0);
    REQUIRE (line.find ("will be copied when the card is back") != std::string::npos);
    REQUIRE (line.find (where) != std::string::npos);

    r.outcome = Outcome::CardFull;
    REQUIRE (describe (r, where).find ("This take is on the card, but a file added to it since isn't yet. "
                                       "The card is full.") == 0);

    r.outcome = Outcome::Copied;
    r.cardFolder = "/Volumes/SD/RECORDINGS/2026-10-14_2000_Show";
    REQUIRE (describe (r, where) == "Copied to the card: /Volumes/SD/RECORDINGS/2026-10-14_2000_Show.");
}

TEST_CASE (TakeCopy_TheComputerCopyGoneIsSaid)
{
    FakeFs fs;
    fs.addFolder (kRoot);

    const auto r = run (fs);
    REQUIRE (r.outcome == Outcome::ComputerCopyMissing);
    REQUIRE (fs.callsUnder ("/cards/") == 0);
    REQUIRE (describe (r, kLocal) == "This take's folder on this computer is gone, so it couldn't be "
                                     "copied to the card.");
}

TEST_CASE (TakeRoute_OnThisComputerGoesStraightThere)
{
    TakeRouteInputs in;
    in.destinationOnThisComputer = 1;
    in.computerFreeBytes = 1;
    REQUIRE (decideTakeRoute (in) == TakeRoute::StraightToDestination);
}

TEST_CASE (TakeRoute_OffThisComputerIsCopied)
{
    TakeRouteInputs in;
    in.destinationOnThisComputer = 0;
    in.computerFreeBytes = 500LL * 1024 * 1024 * 1024;
    in.bytesPerSecond = 1024.0 * 1024 * 1024 / 3600.0;
    REQUIRE (decideTakeRoute (in) == TakeRoute::ComputerThenCard);
}

// A card that is not plugged in cannot be identified. It is not this computer.
TEST_CASE (TakeRoute_UnknownIsCopied)
{
    TakeRouteInputs in;
    in.destinationOnThisComputer = -1;
    in.computerFreeBytes = 500LL * 1024 * 1024 * 1024;
    REQUIRE (decideTakeRoute (in) == TakeRoute::ComputerThenCard);
}

// Mutation: no bound on the wait. A card hung before the probe's first answer
// kept Record on "Checking this save location..." for good, though the take
// would only have been written on this computer.
TEST_CASE (TakeRoute_ACardThatNeverAnswersStopsHoldingRecordAfterTwoSeconds)
{
    PlacementWait wait;
    REQUIRE_FALSE (wait.givenUp ("/Volumes/SD", false, 1000.0));
    REQUIRE_FALSE (wait.givenUp ("/Volumes/SD", false, 1000.0 + PlacementWait::kGiveUpAfterMs));
    REQUIRE (wait.givenUp ("/Volumes/SD", false, 1001.0 + PlacementWait::kGiveUpAfterMs));

    // Another save location is waited for afresh.
    REQUIRE_FALSE (wait.givenUp ("/Volumes/OTHER", false, 5000.0));
    REQUIRE (wait.givenUp ("/Volumes/OTHER", false, 7001.0));

    // An answer ends the wait; a later unanswered look starts a new one.
    REQUIRE_FALSE (wait.givenUp ("/Volumes/OTHER", true, 8000.0));
    REQUIRE_FALSE (wait.givenUp ("/Volumes/OTHER", false, 9000.0));
    REQUIRE (wait.givenUp ("/Volumes/OTHER", false, 11001.0));
}

TEST_CASE (TakeRoute_SettingOffGoesStraightToTheCard)
{
    TakeRouteInputs in;
    in.copyAfterTakeEnabled = false;
    in.destinationOnThisComputer = 0;
    in.computerFreeBytes = 500LL * 1024 * 1024 * 1024;
    REQUIRE (decideTakeRoute (in) == TakeRoute::StraightToDestination);
}

// Mutation: < and <= swapped, or the headroom left out.
TEST_CASE (TakeRoute_ComputerFullBoundary)
{
    TakeRouteInputs in;
    in.destinationOnThisComputer = 0;
    in.bytesPerSecond = 3.0 * 1024 * 1024 * 1024 / 3600.0; // 3 GB an hour

    const auto projected = projectedTakeBytes (in);
    REQUIRE (projected == static_cast<int64_t> (in.bytesPerSecond * kPlanningSeconds));

    in.computerFreeBytes = projected + MirrorPolicy::kMinHeadroomBytes;
    REQUIRE (decideTakeRoute (in) == TakeRoute::StraightToCardComputerFull);

    in.computerFreeBytes = projected + MirrorPolicy::kMinHeadroomBytes + 1;
    REQUIRE (decideTakeRoute (in) == TakeRoute::ComputerThenCard);
}

// Mutation: the card's room left out of the projection. A 3-hour show on six
// mics (about 4 GB an hour) with a 64 GB card and 12 GB free here: recorded
// here it would stop at about 2.5 hours for want of room, where the card
// would have held all of it. So it goes to the card.
TEST_CASE (TakeRoute_NeverStopsATakeTheCardWouldHaveHeld)
{
    constexpr double gb = 1024.0 * 1024 * 1024;

    TakeRouteInputs in;
    in.destinationOnThisComputer = 0;
    in.bytesPerSecond = 4.0 * gb / 3600.0;
    in.destinationRemainingSeconds = 60.0 * gb / in.bytesPerSecond; // 15 hours
    in.computerFreeBytes = static_cast<int64_t> (12.0 * gb);
    REQUIRE (projectedTakeBytes (in) == static_cast<int64_t> (in.bytesPerSecond * kLongestShowSeconds));
    REQUIRE (decideTakeRoute (in) == TakeRoute::StraightToCardComputerFull);

    // Room here for the longest show: recorded here.
    in.computerFreeBytes = projectedTakeBytes (in) + MirrorPolicy::kMinHeadroomBytes + 1;
    REQUIRE (decideTakeRoute (in) == TakeRoute::ComputerThenCard);

    // A card with less room than that: this computer needs only what the
    // card could have held, at the same boundary.
    in.destinationRemainingSeconds = 2.0 * 3600.0;
    const auto cardHolds = static_cast<int64_t> (in.bytesPerSecond * 2.0 * 3600.0);
    REQUIRE (projectedTakeBytes (in) == cardHolds);
    in.computerFreeBytes = cardHolds + MirrorPolicy::kMinHeadroomBytes;
    REQUIRE (decideTakeRoute (in) == TakeRoute::StraightToCardComputerFull);
    in.computerFreeBytes = cardHolds + MirrorPolicy::kMinHeadroomBytes + 1;
    REQUIRE (decideTakeRoute (in) == TakeRoute::ComputerThenCard);

    // The card out: recording here is the only way, and an hour is enough.
    in.destinationRemainingSeconds.reset();
    REQUIRE (projectedTakeBytes (in) == static_cast<int64_t> (in.bytesPerSecond * kPlanningSeconds));
}

// Not knowing how much room there is does not send the take to the card.
TEST_CASE (TakeRoute_FreeSpaceUnknownIsCopied)
{
    TakeRouteInputs in;
    in.destinationOnThisComputer = 0;
    in.bytesPerSecond = 3.0 * 1024 * 1024 * 1024 / 3600.0;
    REQUIRE (decideTakeRoute (in) == TakeRoute::ComputerThenCard);
}

TEST_CASE (TakeCopyWording_EveryFailureNamesWhereTheTakeIsInPlainWords)
{
    const std::string where = "/Users/sam/RECORDINGS-MIRROR/2026-10-14_2000_Show";

    for (const auto outcome : { Outcome::NotFinishedYet, Outcome::CardMissing, Outcome::CardFull,
                                Outcome::CardLocked, Outcome::NotAllowed, Outcome::FileTooBig,
                                Outcome::NameTakenOnCard, Outcome::CopyDidNotMatch,
                                Outcome::CardWriteFailed, Outcome::ComputerCopyMissing,
                                Outcome::ComputerReadFailed })
    {
        Result r;
        r.outcome = outcome;
        r.fileName = "MIX.wav";
        r.osError = 28;
        const auto line = describe (r, where);

        REQUIRE_FALSE (line.empty());

        // The one failure where there is nothing left on the computer to name.
        if (outcome != Outcome::ComputerCopyMissing)
            REQUIRE (line.find (where) != std::string::npos);

        for (const char* jargon : { "errno", "ENOSPC", "EIO", "I/O", "error code", "0x", "28" })
            if (line.find (jargon) != std::string::npos)
                mmatest::failAt (__FILE__, __LINE__, "jargon in: " + line);
    }

    Result copied;
    copied.outcome = Outcome::Copied;
    copied.cardFolder = "/Volumes/SD/RECORDINGS/2026-10-14_2000_Show";
    REQUIRE (describe (copied, where) == "Copied to the card: /Volumes/SD/RECORDINGS/2026-10-14_2000_Show.");

    for (const auto quiet : { Outcome::Yielded, Outcome::Cancelled })
    {
        Result r;
        r.outcome = quiet;
        REQUIRE (describe (r, where).empty());
        REQUIRE_FALSE (isFailure (quiet));
    }

    REQUIRE (stalledLine (where) == "The card stopped answering. This take is safe on this computer in " + where + ".");
}

// Mutation: the old advice, "Delete old takes in RECORDINGS-MIRROR". Some of
// them may be the only copy of a take the card never got.
TEST_CASE (TakeCopyWording_ComputerFullPointsOnlyAtCopiedTakes)
{
    const std::string folder = "/Users/sam/RECORDINGS-MIRROR";

    const auto none = computerFullNote (folder, 0);
    REQUIRE (none.find ("delete takes in " + folder + " that have already been copied to the card.")
             != std::string::npos);
    REQUIRE (none.find ("Delete old takes") == std::string::npos);
    REQUIRE (none.find ("Keep") == std::string::npos);

    REQUIRE (computerFullNote (folder, 1).find ("1 take there hasn't been copied yet. Keep it.") != std::string::npos);
    REQUIRE (computerFullNote (folder, 3).find ("3 takes there haven't been copied yet. Keep them.")
             != std::string::npos);
}

TEST_CASE (TakeCopyWording_CopyingLineIsAPercentage)
{
    REQUIRE (copyingLine (0, 0) == "Copying to the card: 0%.");
    REQUIRE (copyingLine (40, 100) == "Copying to the card: 40%.");
    REQUIRE (copyingLine (100, 100) == "Copying to the card: 100%.");
    REQUIRE (copyingLine (150, 100) == "Copying to the card: 100%.");
    REQUIRE (copyingLine (1, 3) == "Copying to the card: 33%.");
}

// Mutation: a key renamed on one side only.
TEST_CASE (TakeCopy_Marker_RoundTripAndUnreadable)
{
    const Marker m { "/Volumes/SD/RECORDINGS", "/Volumes/SD/RECORDINGS/2026-10-14_2000_Show_2" };
    const auto back = markerFromJson (markerToJson (m));
    REQUIRE (back.has_value());
    REQUIRE (back->destinationRoot == m.destinationRoot);
    REQUIRE (back->cardFolder == m.cardFolder);

    const auto fresh = markerFromJson (markerToJson ({ "E:\\RECORDINGS", {} }));
    REQUIRE (fresh.has_value());
    REQUIRE (fresh->destinationRoot == "E:\\RECORDINGS");
    REQUIRE (fresh->cardFolder.empty());

    REQUIRE (markerToJson (m).find ("\"destinationRoot\"") != std::string::npos);
    REQUIRE (markerToJson (m).find ("\"cardFolder\"") != std::string::npos);

    REQUIRE_FALSE (markerFromJson ("").has_value());
    REQUIRE_FALSE (markerFromJson ("not json at all").has_value());
    REQUIRE_FALSE (markerFromJson ("{ \"cardFolder\": \"x\" }").has_value());
    REQUIRE_FALSE (markerFromJson ("{ \"destinationRoot\": \"\" }").has_value());
}

TEST_CASE (TakeCopy_PathsJoinAndSplit)
{
    REQUIRE (parentOf ("/a/b/c") == "/a/b");
    REQUIRE (parentOf ("/a/b/c/") == "/a/b");
    REQUIRE (parentOf ("/a") == "/");
    REQUIRE (parentOf ("/") == "/");
    REQUIRE (parentOf ("E:\\RECORDINGS") == "E:\\");
    REQUIRE (parentOf ("E:\\") == "E:\\");
    REQUIRE (lastPartOf ("/a/b/c") == "c");
    REQUIRE (lastPartOf ("/a/b/c/") == "c");
    REQUIRE (lastPartOf ("E:\\RECORDINGS\\Take") == "Take");
    REQUIRE (isWorkingFileName (".MIX.wav.sobcopy"));
    REQUIRE_FALSE (isWorkingFileName ("MIX.wav.sobcopy"));
    REQUIRE_FALSE (isWorkingFileName (".sobcopy"));
}

// Mutation: the card touched before the hold is looked at. A take starting
// while a copy waits to begin must not have the card asked anything at all --
// not even whether it is there.
TEST_CASE (TakeCopy_AHeldCopyMakesNoCallToTheCard)
{
    FakeFs fs;
    finishedTake (fs);

    Control control;
    control.shouldYield = [] { return true; };

    const auto r = run (fs, toTheCard(), &control);
    REQUIRE (r.outcome == Outcome::Yielded);
    REQUIRE (fs.callsUnder ("/cards/") == 0);
}

// Mutation: reading back counted as standing still. A big file read back from
// a slow card takes a while, and must not be called a card that stopped
// answering.
TEST_CASE (TakeCopy_ReadingBackCountsAsMoving)
{
    FakeFs fs;
    finishedTake (fs);
    fs.addFile (local ("V01_Cam.mov"), bytes (3 * kChunkBytes, 4));
    fs.addFile (card ("V01_Cam.mov"), fs.files[local ("V01_Cam.mov")]);

    int ticks = 0;
    Control control;
    control.progress = [&ticks] (uint64_t, uint64_t) { ++ticks; };

    // Everything else already on the card and checked, so the movie is only
    // read back.
    std::set<std::string> verified;
    for (const char* name : { "MIX.wav", "01_Alice.wav", "02_Bob.wav", "activity.log", "session.json" })
    {
        fs.addFile (card (name), fs.files[local (name)]);
        verified.insert (card (name) + "|" + std::to_string (fs.files[local (name)].size()));
    }
    control.verified = &verified;

    REQUIRE (run (fs, { kRoot, kCardTake }, &control).outcome == Outcome::Copied);
    REQUIRE (fs.count ("createNew") == 0);
    REQUIRE (ticks >= 3);
}
