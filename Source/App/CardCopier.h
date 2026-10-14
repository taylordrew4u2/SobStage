#pragma once

#include "../Core/TakeCopy.h"
#include "../Core/TakeFolderTracker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mma {

/// Copies finished takes from this computer to the card, one at a time, on a
/// worker of its own (see cardcopy::copyTake for what one copy does).
///
/// Built like PodcastExporter: one detached worker holding a share of the
/// state, so a card that stops answering can hold it inside one call for as
/// long as it likes without the window, Stop or quitting waiting on it. It
/// runs at the lowest disk priority the system offers, and never while a take
/// is recording (setHeld): the take's own writes come first.
///
/// Every method is for the message thread.
class CardCopier
{
public:
    struct Job
    {
        /// The take's folder on this computer (UTF-8).
        std::string localFolder;
        cardcopy::Marker marker;
        /// The take's folder itself, so a copy follows it through a rename
        /// in Finder. Null for a take found at launch.
        std::shared_ptr<TakeFolderTracker> followedFolder;
    };

    enum class Phase { None, Waiting, Copying, Copied, StillOnComputer };

    struct View
    {
        Phase phase = Phase::None;
        uint64_t done = 0, total = 0;
        /// The last finished attempt, when there has been one.
        cardcopy::Result result;
        /// Where the take is on this computer now, followed through a rename.
        std::string folderNow;
    };

    struct Finished
    {
        /// The folder the job was started with: what the app calls the take.
        std::string localFolder;
        /// Where it was when the copy finished.
        std::string folderNow;
        cardcopy::Result result;
        /// The same outcome as this take's last attempt -- a retry that found
        /// the card still missing -- which is not news.
        bool repeat = false;
    };

    using FileSystemFactory = std::function<std::unique_ptr<cardcopy::CopyFileSystem>()>;

    /// `makeFileSystem` makes the disk access each copy uses; by default the
    /// real one (NativeCopyFileSystem). sim_card_copy hands in one it can hold.
    explicit CardCopier (FileSystemFactory makeFileSystem = {});

    /// Cancels, and waits a short bounded while for the worker to clear away
    /// a half-written file. Never joins.
    ~CardCopier();

    CardCopier (const CardCopier&) = delete;
    CardCopier& operator= (const CardCopier&) = delete;

    /// Queues one take. A take already queued or being copied is not queued
    /// twice. False when quitting.
    bool start (Job job);

    /// While held no copy starts, and one under way stops at its next chunk,
    /// removes its half-written file and waits at the front of the queue --
    /// not a failure. Held for the whole of every take.
    void setHeld (bool held);

    /// Finished attempts since the last call, oldest first. Yielded and
    /// cancelled attempts are not among them.
    std::vector<Finished> collectFinished();

    /// Anything queued or being copied, held or not.
    bool isRunning() const;

    /// A copy actually under way: in flight, and not held.
    bool isCopying() const;

    /// How long the copy under way has gone without moving, or 0 when none is.
    double msSinceProgress() const;

    View view (const std::string& localFolder) const;

    /// Queues again every take whose last attempt failed and matches.
    int retry (const std::function<bool (const cardcopy::Result&)>& shouldRetry);

    /// The user's "Copy again". False when the take is not one this copier
    /// knows, or it is already queued or under way.
    bool copyAgain (const std::string& localFolder);

    /// Every take already copied goes again, for a combined video or podcast
    /// copy that has since been added to its folder. Files already checked on
    /// the card in this run are not read again, so a take with nothing new
    /// costs no card access at all. A take still being copied goes again once
    /// it finishes.
    int requeueCopied();

    /// At launch: looks in `stagingRoot` (on the worker) for take folders
    /// still marked to be copied, and queues them, oldest first.
    void resumePendingIn (const std::string& stagingRoot);

    /// How many takes resumePendingIn() found, once, after it has looked.
    std::optional<int> takeResumedCount();

    struct Summary
    {
        /// The take being copied, and how far along.
        std::string copyingFolder;
        uint64_t done = 0, total = 0;
        int waiting = 0;
        /// Takes whose last attempt failed: they are only on this computer.
        int onlyOnComputer = 0;
        /// How many of those failed because the card was not plugged in.
        int cardMissing = 0;
    };
    Summary summary() const;

    void cancel() noexcept;
    bool waitUntilIdle (int timeoutMs);

    static constexpr int kShutdownWaitMs = 2000;

    /// After this long without a byte moving, the card is said to have stopped
    /// answering. Generous: a slow card flushing a big file can pause.
    static constexpr double kStalledAfterMs = 15000.0;

private:
    struct Record
    {
        Job job;
        Phase phase = Phase::None;
        uint64_t done = 0, total = 0;
        std::optional<cardcopy::Result> last;
        std::string folderNow;
        /// Asked to go again while it was queued or under way.
        bool againWhenDone = false;
        /// A run made only to pick up something added after a finished copy.
        bool resync = false;
    };

    struct State
    {
        mutable std::mutex lock;
        std::condition_variable changed;
        std::deque<std::string> queued;     // keys into records
        std::map<std::string, Record> records;
        std::vector<Finished> finished;
        std::string inFlight;
        std::string scanRoot;               // a resumePendingIn() still to do
        std::optional<int> resumedCount;
        bool running = false;
        std::atomic<bool> cancelling { false };
        std::atomic<bool> held { false };
        std::chrono::steady_clock::time_point lastProgress;
        /// Worker only.
        std::set<std::string> verified;
        FileSystemFactory makeFileSystem;
    };

    std::shared_ptr<State> state;

    /// Under the lock. True when the caller must start the worker.
    static bool enqueueLocked (State& s, const std::string& key, bool atFront = false);
    void startWorkerIfNeeded (bool needed);

    static void run (std::shared_ptr<State> state);
    static void scan (State& s, cardcopy::CopyFileSystem& fs, const std::string& root);
};

} // namespace mma
