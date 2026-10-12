#include "FilesystemStatusProbe.h"
#include "TakeCompleteness.h"
#include "Utf8Path.h"
#include "../Platform/VolumeCapacity.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace mma {

namespace fs = std::filesystem;

struct FilesystemStatusProbe::State
{
    State (std::chrono::milliseconds refreshInterval, Sampler sampleFunction,
           Snapshot initialSnapshot = {})
        : interval (std::max (std::chrono::milliseconds (1), refreshInterval)),
          sampler (std::move (sampleFunction)),
          latest (std::move (initialSnapshot))
    {
    }

    const std::chrono::milliseconds interval;
    const Sampler sampler;
    mutable std::mutex mutex;
    mutable std::condition_variable condition;
    bool stopping = false;
    bool hasRequest = false;
    uint64_t requestGeneration = 0;
    Request requested;
    Snapshot latest;
};

FilesystemStatusProbe::FilesystemStatusProbe (std::chrono::milliseconds refreshInterval)
    : FilesystemStatusProbe (refreshInterval, &FilesystemStatusProbe::sample)
{
}

FilesystemStatusProbe::FilesystemStatusProbe (std::chrono::milliseconds refreshInterval,
                                              Sampler sampler)
    : state (std::make_shared<State> (refreshInterval, std::move (sampler)))
{
    launchWorker (state);
}

void FilesystemStatusProbe::launchWorker (const std::shared_ptr<State>& shared)
{
    // The OS may never return from a query against a disappearing removable or
    // network volume. The detached worker therefore owns all state it can
    // touch and never refers back to this object's lifetime. There is exactly
    // one worker for each request target. Replacing the target retires this
    // state and gives the new volume its own worker; ordinary refreshes and
    // throughput-only changes keep using this one.
    std::thread worker ([shared]
    {
        try
        {
            run (shared);
        }
        catch (...)
        {
            // Nothing may escape a detached thread. If bookkeeping itself
            // unexpectedly fails, wake support/test waiters and retire this
            // one-worker probe rather than terminating the process.
            try
            {
                const std::lock_guard<std::mutex> guard (shared->mutex);
                shared->stopping = true;
                shared->condition.notify_all();
            }
            catch (...)
            {
            }
        }
    });

    try
    {
        worker.detach();
    }
    catch (...)
    {
        // detach() should only fail for an invalid thread handle. If the
        // thread remains joinable, it cannot yet be in a filesystem call: the
        // constructor has not returned, so no request can have been supplied.
        {
            const std::lock_guard<std::mutex> guard (shared->mutex);
            shared->stopping = true;
        }
        shared->condition.notify_all();
        if (worker.joinable())
            worker.join();
        throw;
    }
}

FilesystemStatusProbe::~FilesystemStatusProbe()
{
    stop();
}

void FilesystemStatusProbe::setRequest (Request request)
{
    std::shared_ptr<State> shared;
    std::shared_ptr<State> retired;
    bool notifyReplacement = false;

    {
        const std::lock_guard<std::mutex> handleGuard (stateHandleMutex);
        shared = state;

        std::unique_lock<std::mutex> stateGuard (shared->mutex);
        if (shared->stopping || (shared->hasRequest && request == shared->requested))
            return;

        const bool targetChanged = shared->hasRequest
            && ! sameFilesystemTargets (request, shared->requested);

        if (! targetChanged)
        {
            shared->requested = std::move (request);
            shared->hasRequest = true;
            ++shared->requestGeneration;
        }
        else
        {
            // A worker trapped on the previous volume cannot service the new
            // take. Start a fresh state before retiring the old one. If the OS
            // refuses the new thread, keep the old state alive and update its
            // request; it may still catch up, and callers continue to see the
            // honest "unknown" snapshot rather than a stale match.
            const auto previousSnapshot = shared->latest;
            const auto interval = shared->interval;
            const auto sampler = shared->sampler;
            const auto replacementGeneration = shared->requestGeneration + 1;
            stateGuard.unlock();

            std::shared_ptr<State> replacement;
            try
            {
                replacement = std::make_shared<State> (interval, sampler, previousSnapshot);
                launchWorker (replacement);
            }
            catch (...)
            {
                stateGuard.lock();
                if (! shared->stopping)
                {
                    shared->requested = std::move (request);
                    shared->hasRequest = true;
                    ++shared->requestGeneration;
                }
                stateGuard.unlock();
                shared->condition.notify_all();
                return;
            }

            {
                const std::lock_guard<std::mutex> replacementGuard (replacement->mutex);
                replacement->requested = std::move (request);
                replacement->hasRequest = true;
                replacement->requestGeneration = replacementGeneration;
            }

            stateGuard.lock();
            shared->stopping = true;
            stateGuard.unlock();

            retired = shared;
            state = replacement;
            shared = std::move (replacement);
            notifyReplacement = true;
        }
    }

    shared->condition.notify_all();
    if (notifyReplacement && retired != nullptr)
        retired->condition.notify_all();
}

FilesystemStatusProbe::Snapshot FilesystemStatusProbe::getSnapshot() const
{
    std::shared_ptr<State> shared;
    {
        const std::lock_guard<std::mutex> handleGuard (stateHandleMutex);
        shared = state;
    }
    const std::lock_guard<std::mutex> guard (shared->mutex);
    return shared->latest;
}

bool FilesystemStatusProbe::waitForRevisionAfter (uint64_t revision, Snapshot& result,
                                                   std::chrono::milliseconds timeout) const
{
    std::shared_ptr<State> shared;
    {
        const std::lock_guard<std::mutex> handleGuard (stateHandleMutex);
        shared = state;
    }
    std::unique_lock<std::mutex> lock (shared->mutex);
    const bool changed = shared->condition.wait_for (lock, timeout, [shared, revision]
    {
        return shared->stopping || shared->latest.revision > revision;
    });

    result = shared->latest;
    return changed && result.revision > revision;
}

void FilesystemStatusProbe::stop()
{
    std::shared_ptr<State> shared;

    {
        const std::lock_guard<std::mutex> handleGuard (stateHandleMutex);
        shared = state;
        const std::lock_guard<std::mutex> guard (shared->mutex);
        if (shared->stopping)
            return;
        shared->stopping = true;
    }

    // Never join here: the worker may currently be inside an unbounded OS
    // filesystem call. It will observe stopping and release its shared state
    // if and when that call returns.
    shared->condition.notify_all();
}

bool FilesystemStatusProbe::sameFilesystemTargets (const Request& a,
                                                   const Request& b) noexcept
{
    return a.destinationPath == b.destinationPath
        && a.sessionFolder == b.sessionFolder
        && a.mirrorPath == b.mirrorPath;
}

FilesystemStatusProbe::Snapshot FilesystemStatusProbe::sample (const Request& request)
{
    Snapshot out;
    out.request = request;
    out.ready = true;
    out.sampledOnThread = std::this_thread::get_id();

    if (! request.destinationPath.empty() && ! request.backupRootPath.empty())
    {
        const auto destination = volumeIdentityOf (request.destinationPath);
        const auto backup = volumeIdentityOf (request.backupRootPath);

        if (mirrorplacement::sharesDisk (destination, backup))
            out.backupSharesDisk = 1;
        else if (! destination.device.empty() && ! backup.device.empty())
            out.backupSharesDisk = 0;
    }

    // Between takes only: during one the destination is the take's own folder
    // on the card, and nothing done with the answer would touch a take.
    if (! request.destinationPath.empty() && request.sessionFolder.empty())
        out.destinationVolume = volumeFingerprintOf (request.destinationPath);

    if (! request.destinationPath.empty() && request.bytesPerSecond > 0.0)
    {
        std::error_code error;
        const fs::path destination = pathFromUtf8 (request.destinationPath);
        if (fs::is_directory (destination, error) && ! error)
        {
            // What a write can actually claim, purgeable space included on
            // APFS -- see availableBytesForRecording. A backup written to the
            // same disk spends the same free space, so it is counted too.
            if (const auto available = availableBytesForRecording (request.destinationPath))
                out.remainingSeconds = mirrorplacement::remainingSeconds (
                    *available, request.bytesPerSecond, request.backupBytesPerSecond,
                    out.backupSharesDisk == 1);
        }
    }

    if (! request.mirrorPath.empty())
    {
        std::error_code error;
        const fs::path mirror = pathFromUtf8 (request.mirrorPath);
        if (fs::is_directory (mirror, error) && ! error)
        {
            const auto available = availableBytesForRecording (request.mirrorPath);
            if (available && *available <= static_cast<uint64_t> (INT64_MAX))
                out.mirrorFreeBytes = static_cast<int64_t> (*available);
        }
    }

    if (! request.sessionFolder.empty())
    {
        std::error_code error;
        const fs::path folder = pathFromUtf8 (request.sessionFolder);
        const bool isDirectory = fs::is_directory (folder, error);

        if (! error && isDirectory)
        {
            fs::directory_iterator iterator (folder, error), end;
            while (! error && iterator != end)
            {
                const auto& entry = *iterator;
                std::error_code entryError;
                const bool isRegularFile = entry.is_regular_file (entryError);
                if (entryError)
                {
                    error = entryError;
                    break;
                }

                // Finder's .DS_Store and an ExFAT card's "._MIX.wav" twins
                // are not part of the take; see isSystemClutterFile.
                if (isRegularFile && ! isSystemClutterFile (utf8FromPath (entry.path().filename())))
                {
                    const auto size = entry.file_size (entryError);
                    if (entryError)
                    {
                        error = entryError;
                        break;
                    }

                    if (size <= static_cast<uintmax_t> (INT64_MAX))
                        out.files.push_back ({ utf8FromPath (entry.path().filename()),
                                               static_cast<int64_t> (size) });
                }

                iterator.increment (error);
            }
        }

        // A missing folder is a valid empty observation; a system-call error
        // is not. Do not expose a partial listing as zero/growth evidence.
        out.filesObservationReady = ! error;
        if (! out.filesObservationReady)
            out.files.clear();

        const auto rank = [] (const std::string& name)
        {
            std::string lower = name;
            std::transform (lower.begin(), lower.end(), lower.begin(), [] (unsigned char c)
            { return static_cast<char> (std::tolower (c)); });
            if (lower.rfind ("mix", 0) == 0) return 0;
            if (lower == "session.json") return 2;
            return 1;
        };

        std::sort (out.files.begin(), out.files.end(), [&rank] (const File& a, const File& b)
        {
            const int ar = rank (a.name), br = rank (b.name);
            return ar != br ? ar < br : a.name < b.name;
        });
    }

    return out;
}

void FilesystemStatusProbe::run (std::shared_ptr<State> shared)
{
    std::unique_lock<std::mutex> lock (shared->mutex);
    shared->condition.wait (lock, [shared]
    {
        return shared->stopping || shared->hasRequest;
    });

    while (! shared->stopping)
    {
        const auto request = shared->requested;
        const auto generation = shared->requestGeneration;

        lock.unlock();
        Snapshot snapshot;
        bool completed = false;
        const auto startedAt = std::chrono::steady_clock::now();

        try
        {
            snapshot = shared->sampler (request);
            snapshot.sampleStartedAt = startedAt;
            completed = true;
        }
        catch (...)
        {
            // An exception must not escape a detached worker and terminate the
            // process. Keep the last confirmed snapshot and retry later.
        }

        lock.lock();

        // Do not publish a slow result for a destination that was replaced
        // while the card was being queried, or any result after stop() has
        // promised the owner that probing is finished.
        if (! shared->stopping && completed && generation == shared->requestGeneration)
        {
            snapshot.revision = shared->latest.revision + 1;
            shared->latest = std::move (snapshot);
            shared->condition.notify_all();
        }

        shared->condition.wait_for (lock, shared->interval, [shared, generation]
        {
            return shared->stopping || generation != shared->requestGeneration;
        });
    }
}

} // namespace mma
