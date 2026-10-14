// Record on this computer, then copy to the card: CardCopier against the real
// disk (NativeCopyFileSystem) in a temporary folder, with every call able to
// be counted and held.
//
// The unit tests drive cardcopy::copyTake through a fake. What they cannot
// show is the worker around it: that a copy held for a take touches the card
// not at all, that one held mid-file stops at a chunk boundary and leaves
// nothing half-written, that quitting while the card is stuck inside a write
// does not wait for it, that a second pass after a combined video reads back
// only what is new, and that a real file-size refusal from the system reaches
// the right words.
//
//   ./sim_card_copy

#include "App/CardCopier.h"
#include "Platform/NativeCopyFileSystem.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if ! defined(_WIN32)
 #include <fcntl.h>
 #include <sys/mman.h>
 #include <sys/resource.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

using namespace mma;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

int failures = 0;
int checks = 0;

void check (bool ok, const std::string& what)
{
    ++checks;
    std::printf ("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (! ok)
        ++failures;
}

std::string readAll (const fs::path& p)
{
    std::ifstream in (p, std::ios::binary);
    return std::string ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
}

void writeBytes (const fs::path& p, size_t n, unsigned seed)
{
    std::mt19937 rng (seed);
    std::string s (n, '\0');
    for (auto& c : s)
        c = static_cast<char> (rng());
    std::ofstream (p, std::ios::binary) << s;
}

constexpr const char* kFinished =
    "{ \"sessionName\": \"Show\", \"startTimestamp\": \"2026-10-14T20:00:00\", "
    "\"stopTimestamp\": \"2026-10-14T20:45:00\" }";

/// Counts and holds calls on the way to the real disk. Shared by every
/// filesystem the copier asks for.
struct Gate
{
    std::mutex lock;
    std::condition_variable changed;

    std::string cardPrefix;
    int cardCalls = 0;
    /// Every call, this computer's included.
    int allCalls = 0;
    int cardReadsPastCache = 0;
    std::vector<std::string> created;
    std::vector<std::string> readBack;

    /// A write to a file whose name contains this blocks, once `afterWrites`
    /// writes have gone to it, until `open` is set.
    std::string blockWritesTo;
    int afterWrites = 1;
    int writesSoFar = 0;
    bool open = true;
    bool blocked = false;

    /// Every write to a file whose name contains this takes `slowWriteMs`:
    /// a slow card, which is not one that has stopped answering.
    std::string slowWritesTo;
    int slowWriteMs = 0;

    void reset()
    {
        const std::lock_guard<std::mutex> l (lock);
        cardCalls = cardReadsPastCache = allCalls = 0;
        created.clear();
        readBack.clear();
        blockWritesTo.clear();
        writesSoFar = 0;
        open = true;
        blocked = false;
    }

    void note (const std::string& path)
    {
        const std::lock_guard<std::mutex> l (lock);
        ++allCalls;
        if (path.compare (0, cardPrefix.size(), cardPrefix) == 0)
            ++cardCalls;
    }

    void beforeWrite (const std::string& path)
    {
        if (! slowWritesTo.empty() && path.find (slowWritesTo) != std::string::npos)
            std::this_thread::sleep_for (std::chrono::milliseconds (slowWriteMs));

        std::unique_lock<std::mutex> l (lock);
        ++cardCalls;

        if (blockWritesTo.empty() || path.find (blockWritesTo) == std::string::npos)
            return;

        if (++writesSoFar <= afterWrites)
            return;

        blocked = true;
        changed.notify_all();
        changed.wait (l, [this] { return open; });
        blocked = false;
    }

    void release()
    {
        {
            const std::lock_guard<std::mutex> l (lock);
            open = true;
        }
        changed.notify_all();
    }

    bool waitUntilBlocked (std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> l (lock);
        return changed.wait_for (l, limit, [this] { return blocked; });
    }
};

class CountingFs : public cardcopy::CopyFileSystem
{
public:
    explicit CountingFs (std::shared_ptr<Gate> g) : gate (std::move (g)) {}

    bool exists (const std::string& p) override { gate->note (p); return real.exists (p); }
    cardcopy::FsResult list (const std::string& f, std::vector<cardcopy::Entry>& e) override { gate->note (f); return real.list (f, e); }
    cardcopy::FsResult makeFolder (const std::string& p) override { gate->note (p); return real.makeFolder (p); }
    std::optional<uint64_t> freeBytes (const std::string& p) override { gate->note (p); return real.freeBytes (p); }
    std::optional<bool> limitedTo4GiBFiles (const std::string& p) override { gate->note (p); return real.limitedTo4GiBFiles (p); }
    std::string filesystemTypeName (const std::string& p) override { gate->note (p); return real.filesystemTypeName (p); }
    cardcopy::FsResult readText (const std::string& p, std::string& t) override { gate->note (p); return real.readText (p, t); }
    cardcopy::FsResult replaceText (const std::string& p, const std::string& t) override { gate->note (p); return real.replaceText (p, t); }
    cardcopy::FsResult rename (const std::string& a, const std::string& b, bool r) override { gate->note (a); return real.rename (a, b, r); }
    cardcopy::FsResult remove (const std::string& p) override { gate->note (p); return real.remove (p); }
    cardcopy::FsResult syncFolder (const std::string& p) override { gate->note (p); return real.syncFolder (p); }

    std::unique_ptr<Reader> openRead (const std::string& p, bool bypass, cardcopy::FsResult& r) override
    {
        gate->note (p);
        if (p.compare (0, gate->cardPrefix.size(), gate->cardPrefix) == 0)
        {
            const std::lock_guard<std::mutex> l (gate->lock);
            gate->readBack.push_back (p);
            if (bypass)
                ++gate->cardReadsPastCache;
        }
        return real.openRead (p, bypass, r);
    }

    struct HeldWriter : Writer
    {
        std::unique_ptr<Writer> inner;
        std::shared_ptr<Gate> gate;
        std::string path;

        cardcopy::FsResult write (const void* d, size_t n) override
        {
            gate->beforeWrite (path);
            return inner->write (d, n);
        }

        cardcopy::FsResult syncAndClose() override { return inner->syncAndClose(); }
    };

    std::unique_ptr<Writer> createNew (const std::string& p, cardcopy::FsResult& r) override
    {
        gate->note (p);
        {
            const std::lock_guard<std::mutex> l (gate->lock);
            gate->created.push_back (p);
        }

        auto inner = real.createNew (p, r);
        if (inner == nullptr)
            return nullptr;

        auto w = std::make_unique<HeldWriter>();
        w->inner = std::move (inner);
        w->gate = gate;
        w->path = p;
        return w;
    }

private:
    std::shared_ptr<Gate> gate;
    NativeCopyFileSystem real;
};

struct Fixture
{
    fs::path base;
    fs::path staging, card;
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();

    Fixture()
    {
        base = fs::temp_directory_path() / ("sim_card_copy_" + std::to_string (std::random_device {}()));
        staging = base / "home" / "RECORDINGS-MIRROR";
        card = base / "CARD" / "RECORDINGS";
        fs::create_directories (staging);
        fs::create_directories (card);
        gate->cardPrefix = (base / "CARD").string();
    }

    ~Fixture()
    {
        std::error_code ec;
        fs::remove_all (base, ec);
    }

    fs::path take (const std::string& name, size_t bigBytes = 0)
    {
        const auto folder = staging / name;
        fs::create_directories (folder);
        writeBytes (folder / "MIX.wav", 3u << 20, 1);
        writeBytes (folder / "01_Alice.wav", 2u << 20, 2);
        if (bigBytes > 0)
            writeBytes (folder / "V01_Cam.mov", bigBytes, 3);
        std::ofstream (folder / "activity.log") << "Recording started.\nRecording stopped.\n";
        std::ofstream (folder / "session.json") << kFinished;
        std::ofstream (folder / cardcopy::kMarkerFileName)
            << cardcopy::markerToJson ({ card.string(), {} });
        return folder;
    }

    CardCopier::FileSystemFactory factory()
    {
        auto g = gate;
        return [g] { return std::make_unique<CountingFs> (g); };
    }

    CardCopier::Job job (const fs::path& folder, bool moreFilesComing = false,
                         std::shared_ptr<TakeFolderTracker> followed = nullptr)
    {
        return { folder.string(), { card.string(), {} }, std::move (followed), moreFilesComing };
    }
};

std::optional<CardCopier::Finished> waitForResult (CardCopier& copier, std::chrono::milliseconds limit)
{
    const auto until = std::chrono::steady_clock::now() + limit;

    while (std::chrono::steady_clock::now() < until)
    {
        auto done = copier.collectFinished();
        if (! done.empty())
            return done.front();
        std::this_thread::sleep_for (10ms);
    }

    return std::nullopt;
}

/// Every file the copy should carry is on the card, byte for byte.
bool sameOnCard (const fs::path& local, const fs::path& onCard)
{
    for (const auto& e : fs::directory_iterator (local))
    {
        const auto name = e.path().filename().string();
        if (! cardcopy::shouldCopy (name))
            continue;
        if (! fs::exists (onCard / name) || readAll (e.path()) != readAll (onCard / name))
            return false;
    }
    return true;
}

bool workingFilesOn (const fs::path& folder)
{
    if (! fs::exists (folder))
        return false;
    for (const auto& e : fs::directory_iterator (folder))
        if (cardcopy::isWorkingFileName (e.path().filename().string()))
            return true;
    return false;
}

#if ! defined(_WIN32)
/// How many of the file's pages this computer holds in memory right now
/// (mincore), or -1 when it cannot be told.
long residentPages (const std::string& path)
{
    const int fd = ::open (path.c_str(), O_RDONLY);
    if (fd < 0)
        return -1;

    struct stat info {};
    if (::fstat (fd, &info) != 0 || info.st_size <= 0)
    {
        ::close (fd);
        return -1;
    }

    const auto length = static_cast<size_t> (info.st_size);
    void* mapped = ::mmap (nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
    ::close (fd);
    if (mapped == MAP_FAILED)
        return -1;

    const auto page = static_cast<size_t> (::sysconf (_SC_PAGESIZE));
   #if defined(__APPLE__)
    std::vector<char> held ((length + page - 1) / page);
   #else
    std::vector<unsigned char> held ((length + page - 1) / page);
   #endif

    long count = -1;
    if (::mincore (mapped, length, held.data()) == 0)
    {
        count = 0;
        for (auto h : held)
            count += (h & 1) != 0 ? 1 : 0;
    }

    ::munmap (mapped, length);
    return count;
}
#endif

} // namespace

int main()
{
   #if ! defined(_WIN32)
    // RLIMIT_FSIZE below: the write that crosses it gets EFBIG, not a signal.
    std::signal (SIGXFSZ, SIG_IGN);
   #endif

    std::printf ("\n1. A real round trip\n");
    {
        Fixture f;
        const auto local = f.take ("2026-10-14_2000_Show");
        const auto before = readAll (local / "MIX.wav");
        // Another take, copied before, with nothing still being made for it.
        const auto earlier = f.take ("2026-10-14_1930_Earlier");

        CardCopier copier (f.factory());
        check (copier.start (f.job (earlier)), "an earlier take is queued");
        check (waitForResult (copier, 15s).has_value(), "and copied");

        // A combined video is still being made for this one.
        check (copier.start (f.job (local, true)), "the take is queued");
        const auto r = waitForResult (copier, 15s);

        check (r.has_value() && r->result.outcome == cardcopy::Outcome::Copied, "the take is copied");
        const auto onCard = f.card / "2026-10-14_2000_Show";
        check (r.has_value() && r->result.cardFolder == onCard.string(), "into a folder of the same name");
        check (sameOnCard (local, onCard), "every file on the card matches the computer's, byte for byte");
        check (! workingFilesOn (onCard), "no .sobcopy file is left on the card");
        check (fs::exists (local / cardcopy::kMarkerFileName),
               "the marker stays on the computer while its combined video is being made");
        check (fs::exists (local / "MIX.wav") && readAll (local / "MIX.wav") == before,
               "the computer's copy is still there, unchanged");
        check (f.gate->cardReadsPastCache == 8, "each of the 4 files of both takes was read back past the cache");
        check (! copier.isRunning(), "nothing is left running");
        check (! copier.hasWorkToFinish(), "and nothing a quit would cut off");

        // 5. The combined video arrives after the copy: only it goes, and
        //    only for the take it was made for.
        std::printf ("\n5. Another pass after a combined video is added\n");
        writeBytes (local / "V01_Cam_with-sound.mov", 1u << 20, 9);
        fs::remove_all (earlier); // deleted from this computer once on the card
        f.gate->reset();
        check (copier.followUpsFinished() == 1, "only the take it was made for goes again");
        check (copier.hasWorkToFinish(), "which a quit now waits for");
        const auto again = waitForResult (copier, 15s);
        check (again.has_value() && again->result.outcome == cardcopy::Outcome::Copied
                   && again->localFolder == local.string(), "and is copied");
        check (f.gate->created.size() == 1u
                   && f.gate->created.front().find ("V01_Cam_with-sound.mov") != std::string::npos,
               "only the new file is written");
        check (f.gate->readBack.size() == 1u
                   && f.gate->readBack.front().find ("V01_Cam_with-sound.mov") != std::string::npos,
               "and the card is read 0 times for the files already checked");
        check (sameOnCard (local, onCard), "the card has the combined video too");
        check (! fs::exists (local / cardcopy::kMarkerFileName), "and only now is the marker gone");
        check (copier.collectFinished().empty(), "the deleted earlier take is not said to be missing");
        check (copier.followUpsFinished() == 0, "and nothing goes twice");
    }

    std::printf ("\n5b. A combined video that finishes with the card out\n");
    {
        Fixture f;
        const auto local = f.take ("2026-10-14_2015_Out");

        CardCopier copier (f.factory());
        copier.start (f.job (local, true));
        check (waitForResult (copier, 15s).has_value(), "the take is copied");

        writeBytes (local / "V01_Cam_with-sound.mov", 1u << 20, 9);
        const auto cardRoot = f.card.parent_path();
        fs::rename (cardRoot, f.base / "CARD-out");
        copier.followUpsFinished();
        const auto r = waitForResult (copier, 15s);

        check (r.has_value() && r->result.outcome == cardcopy::Outcome::CardMissing, "the card is out");
        check (r.has_value() && r->result.addedFilesOnly, "and it is said to be only the added file");
        check (r.has_value() && cardcopy::describe (r->result, local.string()).find ("This take is on the card") == 0,
               "in words that do not call the take only on this computer");
        check (copier.summary().onlyOnComputer == 0 && copier.summary().addedFilesWaiting == 1,
               "the status line counts it as a file still to copy");
        check (fs::exists (local / cardcopy::kMarkerFileName), "the marker is there for the next launch");
        check (! copier.hasWorkToFinish(), "a quit does not wait for a card that is out");

        copier.setHeld (true);
        check (copier.retry ([] (const cardcopy::Result&) { return true; }) == 1, "the retry is queued");
        check (! copier.hasWorkToFinish(), "nor for a retry of it, queued and not yet tried");
        copier.setHeld (false);
        check (waitForResult (copier, 15s).has_value(), "the retry finds the card still out");
        fs::rename (f.base / "CARD-out", cardRoot);
    }

    std::printf ("\n2. Held before it starts\n");
    {
        Fixture f;
        const auto local = f.take ("2026-10-14_2100_Held");

        CardCopier copier (f.factory());
        copier.setHeld (true);
        copier.start (f.job (local));
        std::this_thread::sleep_for (500ms);

        check (f.gate->cardCalls == 0, "a held copy makes no call to the card at all");
        check (f.gate->allCalls == 0, "and waits rather than trying again and again (no calls at all)");
        check (copier.isRunning(), "it is waiting");
        check (! copier.isCopying(), "and is not counted as copying");
        check (copier.hasWorkToFinish(), "but a quit still counts it as copying to finish");
        check (copier.summary().waiting == 1, "the status line counts it as waiting");
        check (copier.msHeld() >= 400.0, "and how long it has been held is known");
        check (copier.view (local.string()).phase == CardCopier::Phase::Waiting, "its take reads as waiting");

        copier.setHeld (false);
        const auto r = waitForResult (copier, 15s);
        check (r.has_value() && r->result.outcome == cardcopy::Outcome::Copied, "released, it is copied");
    }

    std::printf ("\n3. Held in the middle of a file\n");
    {
        Fixture f;
        const auto local = f.take ("2026-10-14_2200_Mid", 3 * cardcopy::kChunkBytes);
        const auto onCard = f.card / "2026-10-14_2200_Mid";

        {
            const std::lock_guard<std::mutex> l (f.gate->lock);
            f.gate->blockWritesTo = "V01_Cam.mov";
            f.gate->afterWrites = 1;
            f.gate->open = false;
        }

        CardCopier copier (f.factory());
        copier.start (f.job (local));
        check (f.gate->waitUntilBlocked (15s), "the copy reaches the movie's second chunk");

        // 6. While it is stuck, it is copying and not moving.
        std::printf ("\n6. The time since it last moved grows while it is stuck\n");
        check (copier.isCopying(), "it counts as copying");
        const auto first = copier.msSinceProgress();
        std::this_thread::sleep_for (300ms);
        const auto second = copier.msSinceProgress();
        check (second >= first + 250.0, "msSinceProgress grew by the time waited ("
                                            + std::to_string (static_cast<int> (second - first)) + " ms)");

        std::printf ("\n3. (continued)\n");
        copier.setHeld (true);
        f.gate->release();

        // The held copy stops at the next chunk and goes back to waiting.
        const auto until = std::chrono::steady_clock::now() + 10s;
        while (copier.isCopying() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for (10ms);

        std::this_thread::sleep_for (200ms);
        check (! copier.isCopying(), "a take starting stops the copy within a chunk");
        check (! workingFilesOn (onCard), "and the half-written movie is removed from the card");
        check (! fs::exists (onCard / "V01_Cam.mov"), "nothing has the movie's real name");
        check (copier.collectFinished().empty(), "being held is not a failure to report");
        check (copier.view (local.string()).phase == CardCopier::Phase::Waiting, "the take waits for its turn");

        copier.setHeld (false);
        const auto r = waitForResult (copier, 20s);
        check (r.has_value() && r->result.outcome == cardcopy::Outcome::Copied, "released, it finishes");
        check (sameOnCard (local, onCard), "and the card matches, movie included");
    }

    std::printf ("\n3b. The take's folder renamed on this computer mid-copy\n");
    {
       #if defined(__linux__) || defined(__APPLE__)
        Fixture f;
        const auto local = f.take ("2026-10-14_2230_Renamed", 3 * cardcopy::kChunkBytes);
        const auto renamed = f.staging / "Show - Kai";

        auto tracker = std::make_shared<TakeFolderTracker>();
        check (tracker->open (local.string()), "the folder is held");

        {
            const std::lock_guard<std::mutex> l (f.gate->lock);
            f.gate->blockWritesTo = "V01_Cam.mov";
            f.gate->afterWrites = 1;
            f.gate->open = false;
        }

        CardCopier copier (f.factory());
        copier.start (f.job (local, false, tracker));
        check (f.gate->waitUntilBlocked (15s), "the copy is part way through the movie");
        check (copier.view (local.string()).folderNow == local.string(),
               "while it runs, it names the folder where it is");

        fs::rename (local, renamed);
        f.gate->release();

        const auto r = waitForResult (copier, 20s);
        check (r.has_value() && r->result.outcome == cardcopy::Outcome::Copied,
               "the copy follows the folder and finishes");
        check (r.has_value() && r->folderNow == renamed.string(), "and names it by its new name");
        check (sameOnCard (renamed, f.card / "2026-10-14_2230_Renamed"), "every file reached the card");
       #else
        check (true, "(a folder with open files cannot be renamed on Windows)");
       #endif
    }

    std::printf ("\n4. Quitting while the card is stuck inside a write\n");
    {
        Fixture f;
        const auto local = f.take ("2026-10-14_2300_Quit", 2 * cardcopy::kChunkBytes);
        const auto onCard = f.card / "2026-10-14_2300_Quit";

        {
            const std::lock_guard<std::mutex> l (f.gate->lock);
            f.gate->blockWritesTo = "V01_Cam.mov";
            f.gate->afterWrites = 1;
            f.gate->open = false;
        }

        auto copier = std::make_unique<CardCopier> (f.factory());
        copier->start (f.job (local));
        check (f.gate->waitUntilBlocked (15s), "the copy is stuck in a write");

        const auto began = std::chrono::steady_clock::now();
        copier.reset();
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::steady_clock::now() - began);
        std::printf ("  [measured] the copier let go in %lld ms\n", static_cast<long long> (took.count()));
        check (took <= 2200ms, "quitting does not wait for the stuck card (at most 2.2 s)");

        // The card answers again: the abandoned worker sees it was cancelled
        // and clears away its half-written file.
        f.gate->release();
        std::this_thread::sleep_for (500ms);
        check (! workingFilesOn (onCard), "once the card answers, no half-written file is left");
        check (fs::exists (local / cardcopy::kMarkerFileName), "the marker stays, for the next launch");
    }

    std::printf ("\n7. A file-size refusal from the system itself\n");
    {
       #if ! defined(_WIN32)
        Fixture f;
        const auto local = f.take ("2026-10-14_2330_Big");

        rlimit old {};
        getrlimit (RLIMIT_FSIZE, &old);
        rlimit small = old;
        small.rlim_cur = 1u << 20; // MIX.wav and the stem are bigger than this
        setrlimit (RLIMIT_FSIZE, &small);

        NativeCopyFileSystem real;
        cardcopy::Control control;
        const auto r = cardcopy::copyTake (real, local.string(), { f.card.string(), {} }, control);

        setrlimit (RLIMIT_FSIZE, &old);

        check (r.outcome == cardcopy::Outcome::FileTooBig, "EFBIG reads as a file too big for the card");
        check (! workingFilesOn (f.card / "2026-10-14_2330_Big"), "and the part written is removed");
        check (cardcopy::describe (r, local.string()).find ("too big for the way this card is formatted")
                   != std::string::npos,
               "and the user is told so in plain words");
       #else
        check (true, "(RLIMIT_FSIZE is POSIX)");
       #endif

        // An entry that cannot be looked at -- gone since the folder was read,
        // or a link to nothing -- is left out of a listing, not a reason to
        // call the whole card unreadable.
        {
            const auto folder = f.base / "listing";
            fs::create_directories (folder);
            std::ofstream (folder / "MIX.wav") << "x";
            std::error_code ec;
            fs::create_symlink (folder / "nothing-here", folder / "dangling", ec);

            NativeCopyFileSystem listing;
            std::vector<cardcopy::Entry> entries;
            const auto r = listing.list (folder.string(), entries);
            check (r.ok() && entries.size() == 1 && entries.front().name == "MIX.wav",
                   "a listing with an entry that cannot be looked at still lists the rest");
        }

        check (cardcopy::classifyPosixError (ENOSPC) == cardcopy::FsError::NoSpace, "ENOSPC is a full card");
        check (cardcopy::classifyPosixError (EROFS) == cardcopy::FsError::ReadOnly, "EROFS is a locked card");
        check (cardcopy::classifyPosixError (EPERM) == cardcopy::FsError::NotAllowed,
               "EPERM, the macOS privacy refusal, is not allowed");
        check (cardcopy::classifyPosixError (ENODEV) == cardcopy::FsError::NotFound, "ENODEV is a card gone");
        check (cardcopy::classifyWindowsError (21) == cardcopy::FsError::NotFound, "ERROR_NOT_READY is a card gone");
        check (cardcopy::classifyWindowsError (19) == cardcopy::FsError::ReadOnly, "ERROR_WRITE_PROTECT is locked");
        check (cardcopy::classifyWindowsError (112) == cardcopy::FsError::NoSpace, "ERROR_DISK_FULL is full");
        check (cardcopy::classifyWindowsError (5) == cardcopy::FsError::NotAllowed, "ERROR_ACCESS_DENIED is not allowed");
        check (cardcopy::classifyWindowsError (223) == cardcopy::FsError::TooBig, "ERROR_FILE_TOO_LARGE is too big");
    }

    std::printf ("\n9. A slow card is not a stopped one\n");
    {
        Fixture f;
        const auto local = f.take ("2026-10-14_2345_Slow", 6 * cardcopy::kChunkBytes);
        f.gate->slowWritesTo = "V01_Cam.mov";
        f.gate->slowWriteMs = 250;

        CardCopier copier (f.factory());
        copier.start (f.job (local));

        double longest = 0.0;
        std::optional<CardCopier::Finished> r;
        const auto began = std::chrono::steady_clock::now();

        while (! r.has_value() && std::chrono::steady_clock::now() - began < 20s)
        {
            longest = std::max (longest, copier.msSinceProgress());
            auto done = copier.collectFinished();
            if (! done.empty())
                r = done.front();
            std::this_thread::sleep_for (20ms);
        }

        const auto took = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::steady_clock::now() - began);
        std::printf ("  [measured] the copy took %lld ms; the longest it went without moving was %d ms\n",
                     static_cast<long long> (took.count()), static_cast<int> (longest));
        check (r.has_value() && r->result.outcome == cardcopy::Outcome::Copied, "the slow card is copied to");
        check (took >= 1500ms, "and the copy took as long as a slow card makes it");
        check (longest < 900.0, "but every chunk counted as moving, so it was never near being called stopped");
    }

    std::printf ("\n8. Takes left marked at the last quit are found at launch\n");
    {
        Fixture f;
        const auto older = f.take ("2026-10-14_1900_Older");
        const auto newer = f.take ("2026-10-14_2000_Newer");
        fs::create_directories (f.staging / "2026-10-14_1800_AlreadyCopied");
        std::ofstream (f.staging / "2026-10-14_1800_AlreadyCopied" / "session.json") << kFinished;

        CardCopier copier (f.factory());
        copier.resumePendingIn (f.staging.string());

        std::vector<CardCopier::Finished> done;
        const auto until = std::chrono::steady_clock::now() + 20s;
        while (done.size() < 2 && std::chrono::steady_clock::now() < until)
        {
            for (auto& d : copier.collectFinished())
                done.push_back (d);
            std::this_thread::sleep_for (10ms);
        }

        check (copier.takeResumedCount() == std::optional<int> (2), "two marked takes are found");
        check (done.size() == 2u && done[0].localFolder == older.string() && done[1].localFolder == newer.string(),
               "and copied oldest first");
        check (done.size() == 2u && done[0].result.outcome == cardcopy::Outcome::Copied
                   && done[1].result.outcome == cardcopy::Outcome::Copied,
               "both reach the card");
        check (! fs::exists (f.card / "2026-10-14_1800_AlreadyCopied"), "a take with no marker is left alone");
    }

    std::printf ("\n10. The read-back reads the card, not this computer's memory\n");
    {
       #if ! defined(_WIN32)
        // A card that dropped or garbled a write is caught only if the
        // read-back reaches it. Pages of the file still held in memory from
        // writing it are what an uncached read on a Mac is served instead,
        // so none may be held by the time the read-back starts.
        Fixture f;
        NativeCopyFileSystem disk;
        const auto path = (f.card / ".readback.bin.sobcopy").string();

        cardcopy::FsResult r;
        auto writer = disk.createNew (path, r);
        std::string block (8u << 20, '\0');
        std::mt19937 rng (5);
        for (auto& c : block)
            c = static_cast<char> (rng());
        check (writer != nullptr && writer->write (block.data(), block.size()).ok()
                   && writer->syncAndClose().ok(), "8 MB written and flushed");
        writer.reset();

        auto reader = disk.openRead (path, true, r);
        const auto held = residentPages (path);
        std::printf ("  [measured] pages of it held in memory as the read-back starts: %ld\n", held);
        check (reader != nullptr && held == 0, "none of it is held in memory when the read-back starts");

        std::string back (block.size(), '\0');
        size_t got = 0, total = 0;
        while (reader != nullptr && reader->read (back.data() + total, back.size() - total, got).ok() && got > 0)
            total += got;
        check (total == block.size() && back == block, "and what is read back is what was written");
       #else
        check (true, "(Windows reads back with FILE_FLAG_NO_BUFFERING, which never reads its cache)");
       #endif
    }

    std::printf ("\n%s (%d checks, %d failing)\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILED", checks, failures);
    return failures == 0 ? 0 : 1;
}
