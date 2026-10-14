#include "TestFramework.h"
#include "Core/FilesystemStatusProbe.h"
#include "Platform/VolumeCapacity.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

using namespace mma;

namespace mma {

struct FilesystemStatusProbeTestAccess
{
    static std::unique_ptr<FilesystemStatusProbe> create (
        std::chrono::milliseconds refreshInterval,
        std::function<FilesystemStatusProbe::Snapshot (
            const FilesystemStatusProbe::Request&)> sampler)
    {
        return std::unique_ptr<FilesystemStatusProbe> (
            new FilesystemStatusProbe (refreshInterval, std::move (sampler)));
    }
};

} // namespace mma

namespace {

struct TemporaryFolder
{
    TemporaryFolder()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
                 / ("sobstage-status-probe-" + std::to_string (stamp));
        std::filesystem::create_directories (path);
    }

    ~TemporaryFolder()
    {
        std::error_code ignored;
        std::filesystem::remove_all (path, ignored);
    }

    std::filesystem::path path;
};

void writeBytes (const std::filesystem::path& path, int count)
{
    std::ofstream out (path, std::ios::binary | std::ios::trunc);
    for (int i = 0; i < count; ++i)
        out.put ('x');
}

struct BlockingFilesystemSample
{
    FilesystemStatusProbe::Snapshot run (const FilesystemStatusProbe::Request& request)
    {
        std::unique_lock<std::mutex> lock (mutex);
        ++calls;
        entered = true;
        condition.notify_all();
        condition.wait (lock, [this] { return released; });

        FilesystemStatusProbe::Snapshot snapshot;
        snapshot.request = request;
        snapshot.ready = true;
        snapshot.sampledOnThread = std::this_thread::get_id();
        returned = true;
        condition.notify_all();
        return snapshot;
    }

    bool waitUntilEntered()
    {
        std::unique_lock<std::mutex> lock (mutex);
        return condition.wait_for (lock, std::chrono::seconds (2), [this]
        {
            return entered;
        });
    }

    void release()
    {
        const std::lock_guard<std::mutex> guard (mutex);
        released = true;
        condition.notify_all();
    }

    bool waitUntilReturned()
    {
        std::unique_lock<std::mutex> lock (mutex);
        return condition.wait_for (lock, std::chrono::seconds (2), [this]
        {
            return returned;
        });
    }

    int callCount()
    {
        const std::lock_guard<std::mutex> guard (mutex);
        return calls;
    }

    std::mutex mutex;
    std::condition_variable condition;
    int calls = 0;
    bool entered = false;
    bool released = false;
    bool returned = false;
};

struct ReplacingFilesystemSample
{
    FilesystemStatusProbe::Snapshot run (const FilesystemStatusProbe::Request& request)
    {
        if (request.sessionFolder == "/blocked-volume/old-take")
        {
            std::unique_lock<std::mutex> lock (mutex);
            ++blockedCalls;
            blockedEntered = true;
            condition.notify_all();
            condition.wait (lock, [this] { return releaseBlocked; });
            blockedReturned = true;
            condition.notify_all();
        }
        else
        {
            const std::lock_guard<std::mutex> lock (mutex);
            ++replacementCalls;
            condition.notify_all();
        }

        FilesystemStatusProbe::Snapshot snapshot;
        snapshot.request = request;
        snapshot.ready = true;
        snapshot.remainingSeconds = 42.0;
        snapshot.sampledOnThread = std::this_thread::get_id();
        return snapshot;
    }

    bool waitForBlockedEntry()
    {
        std::unique_lock<std::mutex> lock (mutex);
        return condition.wait_for (lock, std::chrono::seconds (2), [this]
        {
            return blockedEntered;
        });
    }

    void release()
    {
        const std::lock_guard<std::mutex> lock (mutex);
        releaseBlocked = true;
        condition.notify_all();
    }

    bool waitForBlockedReturn()
    {
        std::unique_lock<std::mutex> lock (mutex);
        return condition.wait_for (lock, std::chrono::seconds (2), [this]
        {
            return blockedReturned;
        });
    }

    std::pair<int, int> calls()
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return { blockedCalls, replacementCalls };
    }

    std::mutex mutex;
    std::condition_variable condition;
    int blockedCalls = 0;
    int replacementCalls = 0;
    bool blockedEntered = false;
    bool releaseBlocked = false;
    bool blockedReturned = false;
};

} // namespace

TEST_CASE (FilesystemStatusProbe_SamplesSpaceAndFilesOnItsWorker)
{
    const auto callerThread = std::this_thread::get_id();
    TemporaryFolder folder;
    writeBytes (folder.path / "01_Alice.wav", 123);
    writeBytes (folder.path / "MIX.wav", 456);

    FilesystemStatusProbe probe (std::chrono::milliseconds (20));
    FilesystemStatusProbe::Request request;
    request.destinationPath = folder.path.string();
    request.sessionFolder = folder.path.string();
    request.mirrorPath = folder.path.string();
    request.bytesPerSecond = 1000.0;
    probe.setRequest (request);

    FilesystemStatusProbe::Snapshot snapshot;
    REQUIRE (probe.waitForRevisionAfter (0, snapshot, std::chrono::seconds (2)));
    REQUIRE (snapshot.ready);
    REQUIRE (snapshot.filesObservationReady);
    REQUIRE (snapshot.request == request);
    REQUIRE (snapshot.sampledOnThread != callerThread);
    REQUIRE (snapshot.remainingSeconds >= 0.0);
    REQUIRE (snapshot.mirrorFreeBytes >= 0);
    REQUIRE (snapshot.files.size() == 2);
    REQUIRE (snapshot.files[0].name == std::string ("MIX.wav"));
    REQUIRE (snapshot.files[0].sizeBytes == 456);
    REQUIRE (snapshot.files[1].name == std::string ("01_Alice.wav"));
    REQUIRE (snapshot.files[1].sizeBytes == 123);
}

// macOS puts .DS_Store in any folder Finder shows, and an ExFAT card gives a
// file with extended attributes an AppleDouble twin ("._MIX.wav"). Neither is
// part of the take, and the live file list must not show or count them.
TEST_CASE (FilesystemStatusProbe_LeavesFindersOwnFilesOutOfTheTake)
{
    TemporaryFolder folder;
    writeBytes (folder.path / "MIX.wav", 456);
    writeBytes (folder.path / ".DS_Store", 6148);
    writeBytes (folder.path / "._MIX.wav", 4096);

    FilesystemStatusProbe probe (std::chrono::milliseconds (20));
    probe.setRequest ({ {}, folder.path.string(), {}, 0.0 });

    FilesystemStatusProbe::Snapshot snapshot;
    REQUIRE (probe.waitForRevisionAfter (0, snapshot, std::chrono::seconds (2)));
    REQUIRE (snapshot.filesObservationReady);
    REQUIRE (snapshot.files.size() == 1);
    REQUIRE (snapshot.files[0].name == std::string ("MIX.wav"));
}

TEST_CASE (FilesystemStatusProbe_DoesNotCallACompletedFilesystemErrorAnEmptyFolder)
{
    TemporaryFolder folder;

    // A single component far beyond supported filesystem limits makes the
    // native status query fail rather than proving that the folder is empty.
    const auto invalidPath = (folder.path / std::string (32768, 'x')).string();

    FilesystemStatusProbe probe (std::chrono::milliseconds (20));
    probe.setRequest ({ {}, invalidPath, {}, 0.0 });

    FilesystemStatusProbe::Snapshot snapshot;
    REQUIRE (probe.waitForRevisionAfter (0, snapshot, std::chrono::seconds (2)));
    REQUIRE (snapshot.ready);
    REQUIRE_FALSE (snapshot.filesObservationReady);
    REQUIRE (snapshot.files.empty());
}

TEST_CASE (FilesystemStatusProbe_RefreshesAnActiveFolderWithoutMessageThreadIO)
{
    TemporaryFolder folder;
    const auto mix = folder.path / "MIX.wav";
    writeBytes (mix, 16);

    FilesystemStatusProbe probe (std::chrono::milliseconds (10));
    probe.setRequest ({ folder.path.string(), folder.path.string(), {}, 1000.0 });

    FilesystemStatusProbe::Snapshot before;
    REQUIRE (probe.waitForRevisionAfter (0, before, std::chrono::seconds (2)));
    REQUIRE (before.files.size() == 1);

    writeBytes (mix, 128);
    auto revision = before.revision;
    FilesystemStatusProbe::Snapshot after;
    bool sawGrowth = false;

    for (int attempt = 0; attempt < 20 && ! sawGrowth; ++attempt)
    {
        if (! probe.waitForRevisionAfter (revision, after, std::chrono::milliseconds (100)))
            continue;
        revision = after.revision;
        sawGrowth = ! after.files.empty() && after.files[0].sizeBytes == 128;
    }

    REQUIRE (sawGrowth);
}

TEST_CASE (FilesystemStatusProbe_EventuallyPublishesTheLatestReplacedRequest)
{
    TemporaryFolder folder;
    FilesystemStatusProbe probe (std::chrono::milliseconds (10));

    probe.setRequest ({ "/a/path/that/does/not/exist", {}, {}, 1000.0 });
    probe.setRequest ({ folder.path.string(), {}, {}, 1000.0 });

    FilesystemStatusProbe::Snapshot snapshot;
    uint64_t revision = 0;
    bool sawLatest = false;

    // The first, fast request is allowed to finish before it is replaced. What
    // matters is that the worker promptly catches up and does not remain stuck
    // publishing the obsolete destination.
    for (int attempt = 0; attempt < 20 && ! sawLatest; ++attempt)
    {
        if (! probe.waitForRevisionAfter (revision, snapshot, std::chrono::milliseconds (100)))
            continue;

        revision = snapshot.revision;
        sawLatest = snapshot.request.destinationPath == folder.path.string()
                 && snapshot.remainingSeconds >= 0.0;
    }

    REQUIRE (sawLatest);
}

TEST_CASE (FilesystemStatusProbe_StopWakesTheRefreshWait)
{
    FilesystemStatusProbe probe (std::chrono::seconds (5));
    probe.setRequest ({ {}, {}, {}, 0.0 });

    FilesystemStatusProbe::Snapshot snapshot;
    REQUIRE (probe.waitForRevisionAfter (0, snapshot, std::chrono::seconds (2)));

    const auto started = std::chrono::steady_clock::now();
    probe.stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // Comfortably below the five-second refresh sleep, with enough margin for
    // a heavily loaded sanitizer runner.
    REQUIRE (elapsed < std::chrono::seconds (2));
}

TEST_CASE (FilesystemStatusProbe_StopAndDestructionNeverJoinAStuckSample)
{
    auto blocked = std::make_shared<BlockingFilesystemSample>();
    auto probe = FilesystemStatusProbeTestAccess::create (
        std::chrono::milliseconds (10), [blocked] (const auto& request)
        {
            return blocked->run (request);
        });

    probe->setRequest ({ "/blocked-volume", {}, {}, 1000.0 });
    const bool entered = blocked->waitUntilEntered();

    // Throughput estimates can change as cameras are switched without changing
    // the filesystem target. That ordinary same-root churn must not create a
    // growing set of workers while the one sampler is stuck.
    for (int i = 0; i < 20; ++i)
        probe->setRequest ({ "/blocked-volume", {}, {}, 1000.0 + i });

    const int callsWhileBlocked = blocked->callCount();

    const auto stopStarted = std::chrono::steady_clock::now();
    probe->stop();
    const auto stopElapsed = std::chrono::steady_clock::now() - stopStarted;

    const auto destroyStarted = std::chrono::steady_clock::now();
    probe.reset();
    const auto destroyElapsed = std::chrono::steady_clock::now() - destroyStarted;

    // Let the simulated OS call return after its former owner is gone. The
    // worker must finish entirely through shared state, without touching the
    // destroyed probe or publishing another revision.
    blocked->release();
    const bool samplerReturned = blocked->waitUntilReturned();

    const auto workerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
    while (blocked.use_count() != 1 && std::chrono::steady_clock::now() < workerDeadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));

    REQUIRE (entered);
    REQUIRE (callsWhileBlocked == 1);
    REQUIRE (stopElapsed < std::chrono::seconds (1));
    REQUIRE (destroyElapsed < std::chrono::seconds (1));
    REQUIRE (samplerReturned);
    REQUIRE (blocked.use_count() == 1);
}

TEST_CASE (FilesystemStatusProbe_AWedgedOldVolumeCannotPoisonAReplacementTake)
{
    auto sampler = std::make_shared<ReplacingFilesystemSample>();
    auto probe = FilesystemStatusProbeTestAccess::create (
        std::chrono::milliseconds (10), [sampler] (const auto& request)
        {
            return sampler->run (request);
        });

    probe->setRequest (
        { "/blocked-volume", "/blocked-volume/old-take", {}, 1000.0 });
    REQUIRE (sampler->waitForBlockedEntry());

    // A different destination gets independent worker-owned state. It must be
    // observable before the old syscall returns, or every later take would lose
    // free-space and on-disk-growth monitoring for the rest of the process.
    FilesystemStatusProbe::Request replacement {
        "/healthy-volume", "/healthy-volume/new-take", {}, 2000.0
    };
    probe->setRequest (replacement);

    FilesystemStatusProbe::Snapshot replacementSnapshot;
    REQUIRE (probe->waitForRevisionAfter (
        0, replacementSnapshot, std::chrono::seconds (2)));
    REQUIRE (replacementSnapshot.request == replacement);
    REQUIRE (replacementSnapshot.remainingSeconds == 42.0);
    const auto callsBeforeRelease = sampler->calls();
    REQUIRE (callsBeforeRelease.first == 1);
    REQUIRE (callsBeforeRelease.second >= 1);

    sampler->release();
    REQUIRE (sampler->waitForBlockedReturn());

    // A late result from the retired state cannot replace the healthy take's
    // snapshot even though that old worker still outlived its original request.
    std::this_thread::sleep_for (std::chrono::milliseconds (20));
    REQUIRE (probe->getSnapshot().request == replacement);
}

TEST_CASE (FilesystemStatusProbe_AWedgedOldSessionCannotPoisonTheNextTakeOnTheSameRoot)
{
    auto sampler = std::make_shared<ReplacingFilesystemSample>();
    auto probe = FilesystemStatusProbeTestAccess::create (
        std::chrono::milliseconds (10), [sampler] (const auto& request)
        {
            return sampler->run (request);
        });

    probe->setRequest (
        { "/blocked-volume", "/blocked-volume/old-take", {}, 1000.0 });
    REQUIRE (sampler->waitForBlockedEntry());

    // The volume is unchanged, but the live folder belongs to a new take. A
    // directory enumeration stuck on the old session must not prevent the new
    // session's disk-growth proof from ever arriving.
    FilesystemStatusProbe::Request replacement {
        "/blocked-volume", "/blocked-volume/new-take", {}, 1000.0
    };
    probe->setRequest (replacement);

    FilesystemStatusProbe::Snapshot replacementSnapshot;
    REQUIRE (probe->waitForRevisionAfter (
        0, replacementSnapshot, std::chrono::seconds (2)));
    REQUIRE (replacementSnapshot.request == replacement);
    const auto callsBeforeRelease = sampler->calls();
    REQUIRE (callsBeforeRelease.first == 1);
    REQUIRE (callsBeforeRelease.second >= 1);

    sampler->release();
    REQUIRE (sampler->waitForBlockedReturn());
    std::this_thread::sleep_for (std::chrono::milliseconds (20));
    REQUIRE (probe->getSnapshot().request == replacement);
}

// §6.3: a backup on the take's own disk spends the same free space, so the
// remaining-time figure has to count its writes. Here both live under one
// temporary folder, which is one disk by construction; the backup's folder
// does not exist yet, as ~/RECORDINGS-MIRROR does not before its first take.
TEST_CASE (FilesystemStatusProbe_CountsABackupOnTheSameDiskAgainstRemainingTime)
{
    TemporaryFolder folder;

    FilesystemStatusProbe probe (std::chrono::milliseconds (20));
    FilesystemStatusProbe::Request alone;
    alone.destinationPath = folder.path.string();
    alone.bytesPerSecond = 1000.0;
    alone.backupRootPath = (folder.path / "RECORDINGS-MIRROR").string();
    probe.setRequest (alone);

    FilesystemStatusProbe::Snapshot first;
    REQUIRE (probe.waitForRevisionAfter (0, first, std::chrono::seconds (2)));
    REQUIRE (first.backupSharesDisk == 1);
    REQUIRE (first.remainingSeconds > 0.0);

    // The same take, now with a backup running beside it at the same rate.
    auto withBackup = alone;
    withBackup.backupBytesPerSecond = 1000.0;
    probe.setRequest (withBackup);

    FilesystemStatusProbe::Snapshot second;
    auto seen = first.revision;
    bool caughtUp = false;
    for (int i = 0; i < 20 && ! caughtUp; ++i)
    {
        if (! probe.waitForRevisionAfter (seen, second, std::chrono::seconds (2)))
            break;
        seen = second.revision;
        caughtUp = second.request == withBackup;
    }
    REQUIRE (caughtUp);
    REQUIRE (second.backupSharesDisk == 1);

    // Half the time, give or take what the rest of the system wrote meanwhile.
    const double ratio = second.remainingSeconds / first.remainingSeconds;
    REQUIRE (ratio > 0.49 && ratio < 0.51);
}

TEST_CASE (FilesystemStatusProbe_DoesNotJudgeTheBackupsDiskUnlessAsked)
{
    TemporaryFolder folder;

    FilesystemStatusProbe probe (std::chrono::milliseconds (20));
    FilesystemStatusProbe::Request request;
    request.destinationPath = folder.path.string();
    request.bytesPerSecond = 1000.0;
    probe.setRequest (request);

    FilesystemStatusProbe::Snapshot snapshot;
    REQUIRE (probe.waitForRevisionAfter (0, snapshot, std::chrono::seconds (2)));
    REQUIRE (snapshot.backupSharesDisk == -1);
}

// A card swapped for another with the same name sits at the same path, so
// the probe says which volume is there -- between takes, which is when the
// answer decides whether the old card's drive check still holds.
TEST_CASE (FilesystemStatusProbe_SaysWhichVolumeIsAtTheDestinationBetweenTakes)
{
    TemporaryFolder folder;
    FilesystemStatusProbe probe (std::chrono::milliseconds (20));

    FilesystemStatusProbe::Request between;
    between.destinationPath = folder.path.string();
    between.bytesPerSecond = 1000.0;

    const auto submittedAt = std::chrono::steady_clock::now();
    probe.setRequest (between);

    FilesystemStatusProbe::Snapshot snapshot;
    REQUIRE (probe.waitForRevisionAfter (0, snapshot, std::chrono::seconds (2)));
    REQUIRE (snapshot.request == between);
   #if ! defined (_WIN32)
    REQUIRE (snapshot.destinationVolume.known);
    REQUIRE (! volumesDiffer (snapshot.destinationVolume, volumeFingerprintOf (folder.path.string())));
   #endif

    // When the look began, so evidence older than a drive check's verdict can
    // be told from evidence newer than it.
    REQUIRE (snapshot.sampleStartedAt >= submittedAt);

    // During a take the destination is the take's folder, and nothing is done
    // with the answer: not asked.
    FilesystemStatusProbe::Request during = between;
    during.sessionFolder = folder.path.string();
    probe.setRequest (during);

    const auto before = snapshot.revision;
    for (int attempt = 0; attempt < 20 && ! (snapshot.request == during); ++attempt)
        REQUIRE (probe.waitForRevisionAfter (snapshot.revision, snapshot, std::chrono::seconds (2)));

    REQUIRE (snapshot.revision > before);
    REQUIRE (snapshot.request == during);
    REQUIRE (! snapshot.destinationVolume.known);
}

// Mutation: a card that was out when it was checked for interrupted takes is
// never checked again once plugged in, while takes are recorded on this
// computer (the drive check, the only other sign, never runs then).
TEST_CASE (FilesystemStatusProbe_SaysASaveLocationHasAppeared)
{
    using Clock = std::chrono::steady_clock;
    const auto missedAt = Clock::now();

    FilesystemStatusProbe::Snapshot seen;
    seen.ready = true;
    seen.request.destinationPath = "/Volumes/SD/RECORDINGS";
    seen.remainingSeconds = 3600.0;
    seen.sampleStartedAt = missedAt + std::chrono::milliseconds (500);

    REQUIRE (FilesystemStatusProbe::showsDestinationAfter (seen, "/Volumes/SD/RECORDINGS", missedAt));

    // A look begun before the miss proves nothing.
    auto stale = seen;
    stale.sampleStartedAt = missedAt - std::chrono::milliseconds (1);
    REQUIRE_FALSE (FilesystemStatusProbe::showsDestinationAfter (stale, "/Volumes/SD/RECORDINGS", missedAt));

    // Still not there: no room figure.
    auto absent = seen;
    absent.remainingSeconds = -1.0;
    REQUIRE_FALSE (FilesystemStatusProbe::showsDestinationAfter (absent, "/Volumes/SD/RECORDINGS", missedAt));

    // Another save location, or a look at a take's folder mid-take.
    REQUIRE_FALSE (FilesystemStatusProbe::showsDestinationAfter (seen, "/Volumes/OTHER/RECORDINGS", missedAt));
    auto midTake = seen;
    midTake.request.sessionFolder = "/Volumes/SD/RECORDINGS/2026-10-14_2000_Show";
    REQUIRE_FALSE (FilesystemStatusProbe::showsDestinationAfter (midTake, "/Volumes/SD/RECORDINGS", missedAt));

    auto notReady = seen;
    notReady.ready = false;
    REQUIRE_FALSE (FilesystemStatusProbe::showsDestinationAfter (notReady, "/Volumes/SD/RECORDINGS", missedAt));
}
