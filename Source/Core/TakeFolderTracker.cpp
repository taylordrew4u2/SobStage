#include "TakeFolderTracker.h"
#include "Utf8Path.h"

#include <filesystem>
#include <system_error>
#include <thread>

#if defined (__APPLE__) || defined (__linux__)
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

#if defined (__APPLE__)
 #include <sys/param.h>
#endif

namespace mma {

TakeFolderTracker::~TakeFolderTracker()
{
    close();
}

int TakeFolderTracker::openDescriptor (const std::string& folder)
{
    if (folder.empty())
        return -1;

#if defined (__APPLE__)
    // Event-only: the descriptor exists to be asked where the folder is, and
    // must never be the thing that stops a card from ejecting.
    return ::open (folder.c_str(), O_EVTONLY | O_DIRECTORY | O_CLOEXEC);
#elif defined (__linux__)
    return ::open (folder.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
#else
    return -1;
#endif
}

void TakeFolderTracker::closeDescriptor (int fd)
{
#if defined (__APPLE__) || defined (__linux__)
    if (fd >= 0)
        ::close (fd);
#else
    (void) fd;
#endif
}

int TakeFolderTracker::duplicateDescriptor() const noexcept
{
#if defined (__APPLE__) || defined (__linux__)
    return descriptor >= 0 ? ::fcntl (descriptor, F_DUPFD_CLOEXEC, 0) : -1;
#else
    return -1;
#endif
}

bool TakeFolderTracker::open (const std::string& folder)
{
    return adopt (openDescriptor (folder), folder);
}

bool TakeFolderTracker::adopt (int fd, const std::string& folder)
{
    close();

    if (fd < 0)
        return false;

    descriptor = fd;
    openedPath = folder;
    lastSystemPath.clear();
    lastResolved.clear();
    return true;
}

void TakeFolderTracker::close()
{
    // Let go on a disposable thread: the folder is usually on a card, and a
    // close on a card that has stopped answering can wait as long as it does.
    // Nothing is written through this descriptor, so nothing waits on it.
    if (descriptor >= 0)
        std::thread ([fd = descriptor] { closeDescriptor (fd); }).detach();

    descriptor = -1;
    openedPath.clear();
    lastSystemPath.clear();
    lastResolved.clear();
}

std::string TakeFolderTracker::currentSystemPath() const
{
    if (descriptor < 0)
        return {};

#if defined (__APPLE__) && defined (F_GETPATH)
    char buffer[MAXPATHLEN] = {};
    if (::fcntl (descriptor, F_GETPATH, buffer) != 0)
        return {};

    // macOS keeps answering with the last name a deleted folder had, so the
    // answer only counts while that name still leads to this very folder.
    struct stat held {}, named {};
    if (::fstat (descriptor, &held) != 0 || ::stat (buffer, &named) != 0
        || held.st_dev != named.st_dev || held.st_ino != named.st_ino)
        return {};

    return std::string (buffer);
#elif defined (__linux__)
    const auto link = "/proc/self/fd/" + std::to_string (descriptor);
    char buffer[4096] = {};
    const auto length = ::readlink (link.c_str(), buffer, sizeof (buffer) - 1);
    if (length <= 0)
        return {};

    std::string path (buffer, static_cast<size_t> (length));

    // The kernel's way of saying the folder has no name any more.
    const std::string deleted = " (deleted)";
    if (path.size() >= deleted.size()
        && path.compare (path.size() - deleted.size(), deleted.size(), deleted) == 0)
        return {};

    return path;
#else
    return {};
#endif
}

std::string TakeFolderTracker::fileIdentity() const
{
#if defined (__APPLE__) || defined (__linux__)
    struct stat held {};
    if (descriptor < 0 || ::fstat (descriptor, &held) != 0)
        return {};

    return std::to_string (static_cast<unsigned long long> (held.st_dev)) + ":"
         + std::to_string (static_cast<unsigned long long> (held.st_ino));
#else
    return {};
#endif
}

std::string TakeFolderTracker::resolve() const
{
    const auto now = currentSystemPath();
    if (now.empty())
        return {};

    if (now == lastSystemPath)
        return lastResolved;

    std::string resolved = now;

    if (now == openedPath)
    {
        resolved = openedPath;
    }
    else
    {
        // Same folder reached by another spelling (a symlinked parent) is not
        // a move: keep the spelling the take was given, as WritePipeline does.
        std::error_code ec;
        if (std::filesystem::equivalent (pathFromUtf8 (openedPath), pathFromUtf8 (now), ec) && ! ec)
            resolved = openedPath;
    }

    lastSystemPath = now;
    lastResolved = resolved;
    return resolved;
}

} // namespace mma
