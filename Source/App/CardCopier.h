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
        /// A combined video or podcast copy is still being made in the folder.
        /// The copy keeps the marker on this computer until followUpsFinished()
        /// has had the take copied again with it.
        bool moreFilesComing = false;
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

    /// Queues one take, at its Stop. Every call is a take of its own, even at
    /// a folder an earlier take of this run had: take folders are named by
    /// the minute, so an earlier one renamed or deleted in Finder leaves its
    /// path to a take armed in the same minute. Neither inherits the other's
    /// copy. False when quitting.
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

    /// Copying still to do that quitting would cut off: a copy in flight, held
    /// or not, or a take queued that is not merely waiting to retry a failure
    /// (a card that is out, full or locked). A take queued a moment ago at
    /// Stop counts, before the worker has picked it up.
    bool hasWorkToFinish() const;

    /// How long copying has been held, or 0 when it is not.
    double msHeld() const;

    /// How long the copy under way has gone without moving, or 0 when none is.
    double msSinceProgress() const;

    /// The newest take started at `localFolder`: the one the app means by it.
    View view (const std::string& localFolder) const;

    /// Queues again every take whose last attempt failed and matches.
    int retry (const std::function<bool (const cardcopy::Result&)>& shouldRetry);

    /// The user's "Copy again", for the newest take at `localFolder`. False
    /// when the take is not one this copier knows, or it is already queued or
    /// under way.
    bool copyAgain (const std::string& localFolder);

    /// The combined videos and podcast copies being made are all finished:
    /// every take started with moreFilesComing goes again, to copy what was
    /// added to its folder and let go of its marker. Only those takes; files
    /// already checked on the card in this run are not read again. A take
    /// still being copied goes again once it finishes.
    int followUpsFinished();

    /// At launch: looks in `stagingRoot` (on the worker) for take folders
    /// still marked to be copied, and queues them, oldest first.
    void resumePendingIn (const std::string& stagingRoot);

    /// How many takes resumePendingIn() found, once, after it has looked.
    std::optional<int> takeResumedCount();

    /// The takes whose last attempt failed, counted as cardcopy::Tally says:
    /// only on this computer, on the card but for a file too big for it, or
    /// on the card but for a file added since.
    struct Summary : cardcopy::Tally
    {
        /// The take being copied, and how far along.
        std::string copyingFolder;
        uint64_t done = 0, total = 0;
        int waiting = 0;
    };
    Summary summary() const;

    void cancel() noexcept;
    bool waitUntilIdle (int timeoutMs);

    static constexpr int kShutdownWaitMs = 2000;

    /// After this long without a byte moving, the card is said to have stopped
    /// answering. Generous: a slow card flushing a big file can pause.
    static constexpr double kStalledAfterMs = 15000.0;

private:
    /// One take, for the life of this copier. Never reused.
    using TakeId = uint64_t;

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
        /// Its combined video or podcast copy is still being made.
        bool awaitingFollowUps = false;
        /// What the last attempt that copied it left on the card, by size.
        /// A later attempt that fails says "only a file added since" only
        /// while the card it finds still holds these.
        std::map<std::string, uint64_t> copiedFiles;
        /// Gone again after following the folder through a rename: a bound,
        /// so a folder renamed over and over cannot keep the worker busy.
        int movedFolderRetries = 0;
    };

    struct State
    {
        mutable std::mutex lock;
        std::condition_variable changed;
        std::deque<TakeId> queued;
        std::map<TakeId, Record> records;
        /// The newest take started at each folder on this computer.
        std::map<std::string, TakeId> newestAt;
        TakeId nextId = 1;
        std::vector<Finished> finished;
        TakeId inFlight = 0;                // 0: none
        std::string scanRoot;               // a resumePendingIn() still to do
        std::optional<int> resumedCount;
        bool running = false;
        std::atomic<bool> cancelling { false };
        std::atomic<bool> held { false };
        std::chrono::steady_clock::time_point heldSince;
        std::chrono::steady_clock::time_point lastProgress;
        /// Worker only.
        std::set<std::string> verified;
        FileSystemFactory makeFileSystem;
    };

    std::shared_ptr<State> state;

    /// Under the lock. True when the caller must start the worker.
    static bool enqueueLocked (State& s, TakeId id, bool atFront = false);
    /// Under the lock: a new record for `job`, now the newest at its folder.
    static TakeId addTakeLocked (State& s, Job job);
    /// Under the lock: the newest take at `localFolder`, or nothing.
    static Record* newestLocked (State& s, const std::string& localFolder, TakeId* id = nullptr);
    void startWorkerIfNeeded (bool needed);

    static void run (std::shared_ptr<State> state);
    static void scan (State& s, cardcopy::CopyFileSystem& fs, const std::string& root);
};

} // namespace mma
