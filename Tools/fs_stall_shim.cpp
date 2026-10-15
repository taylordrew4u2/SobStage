/* A recording card that stops answering, on demand, for the real app.
 *
 * A USB card pulled or wedged at the wrong moment does not return an error: the
 * filesystem call that touches it simply never comes back. No fixture on a CI
 * runner behaves like that -- tmpfs, loop devices and full drives all answer,
 * quickly and honestly -- so the message-thread paths that touch the card at
 * Record and Stop were only ever tested against fakes inside one class.
 *
 * This is preloaded into the unmodified app. Every libc call that touches a
 * path under MMA_STALL_PREFIX, or a descriptor opened there, is let through
 * normally until the file MMA_STALL_TRIGGER exists. From then on it blocks --
 * the calling thread sleeps inside the call exactly as it would inside a dead
 * USB mass-storage request -- until the trigger is removed, when it finishes
 * normally. Everything outside the prefix, including the local backup, the
 * settings and the log, is untouched.
 *
 *   MMA_STALL_PREFIX   directory standing in for the card (exact path or below)
 *   MMA_STALL_TRIGGER  while this file exists, the card does not answer
 *
 * A drive can also hold one write and not the others: a slow backup disk still
 * inside one rename when the next write to the same file comes along. With
 *
 *   MMA_STALL_RENAME_ONTO  a file name, e.g. session.json
 *
 * only rename() onto a file of that name under the prefix is held, and only the
 * first one made while the trigger exists; every other call goes straight
 * through. "<trigger>.caught" appears when that rename starts waiting and
 * "<trigger>.released" once it has returned, so a gate can tell that the write
 * it meant to hold was held, and when it has landed. Once it is released, the
 * trigger made again holds the next such rename: a copy retried after the
 * first one failed can be held at the same step.
 *
 *   MMA_STALL_CARD_FREE_MB  the card reports no more free space than this
 *
 *   MMA_STALL_CARD_PREFIX  the card, for the two below, when it is not the
 *                          stalled prefix: a gate that stalls this computer's
 *                          own folder still has a card that reports itself one
 *
 *   MMA_STALL_COMPUTER_PREFIX, MMA_STALL_COMPUTER_FREE_MB_FILE  this
 *                          computer's home folder, and a file holding a size
 *                          in MB: while it holds one, everything under that
 *                          folder but the card reports no more free space than
 *                          that -- this computer "nearly full", on demand
 *
 *   MMA_STALL_CARD_FAT_MB_FILE  a file holding a size in MB: the card reports
 *                               itself a FAT card of that size, which is what
 *                               tells one card from another (cardIdentityOf).
 *                               Another size written there is another card.
 *
 * A card can also be slow rather than dead: SD cards pause for whole seconds
 * now and then while they tidy their insides, and the card's speed test
 * measures in one-second windows to catch exactly that. With
 *
 *   MMA_STALL_SLOW_FILE             a file name, e.g. .sobstage-preflight.tmp
 *   MMA_STALL_SLOW_FIRST_WRITE_MS   how long, in ms, the first write pauses
 *
 * a file of that name opened under the card (open, openat, creat) has its
 * first write (write, pwrite, writev) held that long; every later write, and
 * every other file, goes straight through. With the speed test's file and
 * 4000 ms, the test measures about 0.25 MB/s, well under any rig's bar, and
 * still finishes in about 5 s; the copy to the card is not slowed.
 *
 * The gates' "card" is a folder on this computer's own disk, and whether a
 * take is recorded here and copied, or goes straight to the card, depends on
 * how much room this computer has: an hour of recording plus 2 GB. On one
 * disk the card and this computer are the same, so the gates set this
 * computer's room (MMA_STALL_COMPUTER_FREE_MB_FILE) where it matters; a
 * runner needs an hour plus 2 GB free for the rest. A card reporting a few
 * GB is a card, whatever the disk under it.
 */
// Fortified builds turn open() and friends into inline wrappers, which would
// collide with the definitions below.
#undef _FORTIFY_SOURCE
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace {

template <typename Fn>
Fn real (const char* name)
{
    return reinterpret_cast<Fn> (dlsym (RTLD_NEXT, name));
}

const std::string& prefix()
{
    static const std::string p = [] {
        const char* v = std::getenv ("MMA_STALL_PREFIX");
        std::string s = v != nullptr ? v : "";
        while (s.size() > 1 && s.back() == '/')
            s.pop_back();
        return s;
    }();
    return p;
}

const char* trigger()
{
    static const char* t = std::getenv ("MMA_STALL_TRIGGER");
    return t;
}

// The card, for its size and room (MMA_STALL_CARD_FREE_MB and
// MMA_STALL_CARD_FAT_MB_FILE): MMA_STALL_CARD_PREFIX when set, so a gate can
// stall this computer's own folder (MMA_STALL_PREFIX) while the card still
// reports itself a card; MMA_STALL_PREFIX otherwise.
const std::string& cardPrefix()
{
    static const std::string p = [] {
        const char* v = std::getenv ("MMA_STALL_CARD_PREFIX");
        std::string s = v != nullptr && *v != 0 ? v : prefix();
        while (s.size() > 1 && s.back() == '/')
            s.pop_back();
        return s;
    }();
    return p;
}

bool underGivenPrefix (const char* path, const std::string& given)
{
    if (path == nullptr || given.empty())
        return false;

    std::string p (path);

    if (! p.empty() && p[0] != '/')
    {
        char cwd[4096];
        if (::getcwd (cwd, sizeof (cwd)) == nullptr)
            return false;
        p = std::string (cwd) + "/" + p;
    }

    // The exact directory, or something inside it. "RECORDINGS-MIRROR" beside
    // "RECORDINGS" is the local backup and must never be caught by this.
    return p == given || p.compare (0, given.size() + 1, given + "/") == 0;
}

bool underPrefix (const char* path) { return underGivenPrefix (path, prefix()); }
bool underCardPrefix (const char* path) { return underGivenPrefix (path, cardPrefix()); }

std::mutex& fdMutex() { static std::mutex m; return m; }
std::set<int>& cardFds() { static std::set<int> s; return s; }
// MMA_STALL_SLOW_FILE / MMA_STALL_SLOW_FIRST_WRITE_MS: descriptors of that
// file under the card whose first write is still to come.
std::set<int>& slowFds() { static std::set<int> s; return s; }

bool isCardFd (int fd)
{
    const std::lock_guard<std::mutex> lock (fdMutex());
    return cardFds().count (fd) != 0;
}

void rememberFd (int fd, const char* path)
{
    if (fd < 0 || ! underPrefix (path))
        return;
    const std::lock_guard<std::mutex> lock (fdMutex());
    cardFds().insert (fd);
}

void forgetFd (int fd)
{
    const std::lock_guard<std::mutex> lock (fdMutex());
    cardFds().erase (fd);
    slowFds().erase (fd);
}

const char* slowFileName()
{
    static const char* name = [] {
        const char* v = std::getenv ("MMA_STALL_SLOW_FILE");
        return v != nullptr && *v != 0 ? v : nullptr;
    }();
    return name;
}

long long slowFirstWriteMs()
{
    static const long long ms = [] {
        const char* v = std::getenv ("MMA_STALL_SLOW_FIRST_WRITE_MS");
        return v != nullptr && *v != 0 ? std::atoll (v) : 0LL;
    }();
    return ms;
}

void rememberSlowFd (int fd, const char* path)
{
    if (fd < 0 || slowFileName() == nullptr || slowFirstWriteMs() <= 0 || ! underCardPrefix (path))
        return;

    const char* slash = std::strrchr (path, '/');

    if (std::strcmp (slash != nullptr ? slash + 1 : path, slowFileName()) != 0)
        return;

    const std::lock_guard<std::mutex> lock (fdMutex());
    slowFds().insert (fd);
}

// The first write on such a descriptor pauses, as a card does mid-write.
void pauseIfFirstSlowWrite (int fd)
{
    bool first = false;
    {
        const std::lock_guard<std::mutex> lock (fdMutex());
        first = slowFds().erase (fd) > 0;
    }

    if (first)
        std::this_thread::sleep_for (std::chrono::milliseconds (slowFirstWriteMs()));
}

bool cardIsDead()
{
    static auto realAccess = real<int (*) (const char*, int)> ("access");
    return trigger() != nullptr && realAccess (trigger(), F_OK) == 0;
}

// The whole point: block here, in the call, for as long as the card is gone.
void waitWhileCardIsDead()
{
    while (cardIsDead())
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
}

const char* renameOnto()
{
    static const char* name = [] {
        const char* v = std::getenv ("MMA_STALL_RENAME_ONTO");
        return v != nullptr && *v != 0 ? v : nullptr;
    }();
    return name;
}

// Everything under the prefix stalls, unless only one rename is to be held.
bool stallsEverything()
{
    return renameOnto() == nullptr;
}

void touchPath (const char* path)
{
    if (stallsEverything() && underPrefix (path))
        waitWhileCardIsDead();
}

void touchFd (int fd)
{
    if (stallsEverything() && isCardFd (fd))
        waitWhileCardIsDead();
}

// A rename is being held, or was and the trigger has not been made again.
std::atomic<bool> renameHeld { false };

// MMA_STALL_RENAME_ONTO: true for the one rename to hold -- onto the named
// file under the prefix, while the trigger exists, and the first such since
// the last one held was released.
bool isTheRenameToHold (const char* to)
{
    if (renameOnto() == nullptr || to == nullptr || ! underPrefix (to) || ! cardIsDead())
        return false;

    const char* slash = std::strrchr (to, '/');

    if (std::strcmp (slash != nullptr ? slash + 1 : to, renameOnto()) != 0)
        return false;

    return ! renameHeld.exchange (true);
}

// "<trigger><suffix>", created empty. The trigger is outside the prefix.
void mark (const char* suffix)
{
    static const auto realOpen = real<int (*) (const char*, int, ...)> ("open");
    static const auto realClose = real<int (*) (int)> ("close");

    if (trigger() == nullptr)
        return;

    const std::string path = std::string (trigger()) + suffix;
    const int fd = realOpen (path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd >= 0)
        realClose (fd);
}

mode_t modeArg (int flags, va_list args)
{
    return (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE
               ? static_cast<mode_t> (va_arg (args, int)) : 0;
}

// MMA_STALL_CARD_FREE_MB: free blocks under the prefix capped at that size.
template <typename Stat>
int capCardFree (int result, const char* path, Stat* out, unsigned long blockSize)
{
    static const long long capMb = [] {
        const char* v = std::getenv ("MMA_STALL_CARD_FREE_MB");
        return v != nullptr && *v != 0 ? std::atoll (v) : -1LL;
    }();

    if (result != 0 || out == nullptr || capMb < 0 || blockSize == 0 || ! underCardPrefix (path))
        return result;

    const auto capBlocks = static_cast<unsigned long long> (capMb) * 1024ull * 1024ull / blockSize;

    if (static_cast<unsigned long long> (out->f_bavail) > capBlocks)
        out->f_bavail = capBlocks;
    if (static_cast<unsigned long long> (out->f_bfree) > capBlocks)
        out->f_bfree = capBlocks;

    return result;
}

// MMA_STALL_COMPUTER_FREE_MB_FILE: a file holding a size in MB. While it
// holds one, statfs under MMA_STALL_COMPUTER_PREFIX -- this computer's folder
// for takes -- reports no more free space than that: a gate fills this
// computer up, and empties it again, by writing the file. Read on every call;
// an empty or missing file leaves the answer as it is.
template <typename Stat>
int capComputerFree (int result, const char* path, Stat* out, unsigned long blockSize)
{
    static const std::string computer = [] {
        const char* v = std::getenv ("MMA_STALL_COMPUTER_PREFIX");
        std::string s = v != nullptr ? v : "";
        while (s.size() > 1 && s.back() == '/')
            s.pop_back();
        return s;
    }();
    static const char* file = [] {
        const char* v = std::getenv ("MMA_STALL_COMPUTER_FREE_MB_FILE");
        return v != nullptr && *v != 0 ? v : nullptr;
    }();

    // The card may be a folder inside it -- the gates' cards are under their
    // home -- and is never this computer.
    if (result != 0 || out == nullptr || file == nullptr || blockSize == 0 || ! underGivenPrefix (path, computer)
        || underCardPrefix (path))
        return result;

    static const auto realOpen = real<int (*) (const char*, int, ...)> ("open");
    static const auto realRead = real<ssize_t (*) (int, void*, size_t)> ("read");
    static const auto realClose = real<int (*) (int)> ("close");

    char text[32] = {};
    const int fd = realOpen (file, O_RDONLY);
    if (fd < 0)
        return result;
    const auto got = realRead (fd, text, sizeof (text) - 1);
    realClose (fd);

    if (got <= 0)
        return result;

    const auto capBlocks = static_cast<unsigned long long> (std::atoll (text)) * 1024ull * 1024ull / blockSize;

    if (static_cast<unsigned long long> (out->f_bavail) > capBlocks)
        out->f_bavail = capBlocks;
    if (static_cast<unsigned long long> (out->f_bfree) > capBlocks)
        out->f_bfree = capBlocks;

    return result;
}

// MMA_STALL_CARD_FAT_MB_FILE: statfs under the prefix reports a FAT card the
// size, in MB, that this file holds -- so the card has an identity (a FAT
// card's is its format and size), and a gate puts "another card" in its place
// by writing another size. Read on every call; an empty or missing file
// leaves the answer as it is.
template <typename Stat>
void fakeFatCard (const char* path, Stat* out)
{
    static const char* file = [] {
        const char* v = std::getenv ("MMA_STALL_CARD_FAT_MB_FILE");
        return v != nullptr && *v != 0 ? v : nullptr;
    }();

    if (file == nullptr || out == nullptr || out->f_bsize <= 0 || ! underCardPrefix (path))
        return;

    static const auto realOpen = real<int (*) (const char*, int, ...)> ("open");
    static const auto realRead = real<ssize_t (*) (int, void*, size_t)> ("read");
    static const auto realClose = real<int (*) (int)> ("close");

    char text[32] = {};
    const int fd = realOpen (file, O_RDONLY);
    if (fd < 0)
        return;
    const auto got = realRead (fd, text, sizeof (text) - 1);
    realClose (fd);

    const long long mb = got > 0 ? std::atoll (text) : 0;
    if (mb <= 0)
        return;

    out->f_type = 0x4d44; // MSDOS_SUPER_MAGIC
    out->f_blocks = static_cast<unsigned long long> (mb) * 1024ull * 1024ull
                  / static_cast<unsigned long long> (out->f_bsize);
}

} // namespace

extern "C" {

int open (const char* path, int flags, ...)
{
    va_list args; va_start (args, flags); const auto mode = modeArg (flags, args); va_end (args);
    touchPath (path);
    static const auto fn = real<int (*) (const char*, int, ...)> ("open");
    const int fd = fn (path, flags, mode);
    rememberFd (fd, path);
    rememberSlowFd (fd, path);
    return fd;
}

int open64 (const char* path, int flags, ...)
{
    va_list args; va_start (args, flags); const auto mode = modeArg (flags, args); va_end (args);
    touchPath (path);
    static const auto fn = real<int (*) (const char*, int, ...)> ("open64");
    const int fd = fn (path, flags, mode);
    rememberFd (fd, path);
    rememberSlowFd (fd, path);
    return fd;
}

int openat (int dirfd, const char* path, int flags, ...)
{
    va_list args; va_start (args, flags); const auto mode = modeArg (flags, args); va_end (args);
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, int, ...)> ("openat");
    const int fd = fn (dirfd, path, flags, mode);
    rememberFd (fd, path);
    rememberSlowFd (fd, path);
    return fd;
}

int openat64 (int dirfd, const char* path, int flags, ...)
{
    va_list args; va_start (args, flags); const auto mode = modeArg (flags, args); va_end (args);
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, int, ...)> ("openat64");
    const int fd = fn (dirfd, path, flags, mode);
    rememberFd (fd, path);
    rememberSlowFd (fd, path);
    return fd;
}

int creat (const char* path, mode_t mode)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, mode_t)> ("creat");
    const int fd = fn (path, mode);
    rememberFd (fd, path);
    rememberSlowFd (fd, path);
    return fd;
}

int close (int fd)
{
    touchFd (fd);
    forgetFd (fd);
    static const auto fn = real<int (*) (int)> ("close");
    return fn (fd);
}

ssize_t write (int fd, const void* buf, size_t n)
{
    touchFd (fd);
    pauseIfFirstSlowWrite (fd);
    static const auto fn = real<ssize_t (*) (int, const void*, size_t)> ("write");
    return fn (fd, buf, n);
}

ssize_t pwrite (int fd, const void* buf, size_t n, off_t off)
{
    touchFd (fd);
    pauseIfFirstSlowWrite (fd);
    static const auto fn = real<ssize_t (*) (int, const void*, size_t, off_t)> ("pwrite");
    return fn (fd, buf, n, off);
}

ssize_t pwrite64 (int fd, const void* buf, size_t n, off64_t off)
{
    touchFd (fd);
    pauseIfFirstSlowWrite (fd);
    static const auto fn = real<ssize_t (*) (int, const void*, size_t, off64_t)> ("pwrite64");
    return fn (fd, buf, n, off);
}

ssize_t writev (int fd, const struct iovec* iov, int count)
{
    touchFd (fd);
    pauseIfFirstSlowWrite (fd);
    static const auto fn = real<ssize_t (*) (int, const struct iovec*, int)> ("writev");
    return fn (fd, iov, count);
}

ssize_t read (int fd, void* buf, size_t n)
{
    touchFd (fd);
    static const auto fn = real<ssize_t (*) (int, void*, size_t)> ("read");
    return fn (fd, buf, n);
}

int fsync (int fd)
{
    touchFd (fd);
    static const auto fn = real<int (*) (int)> ("fsync");
    return fn (fd);
}

int fdatasync (int fd)
{
    touchFd (fd);
    static const auto fn = real<int (*) (int)> ("fdatasync");
    return fn (fd);
}

int ftruncate (int fd, off_t length)
{
    touchFd (fd);
    static const auto fn = real<int (*) (int, off_t)> ("ftruncate");
    return fn (fd, length);
}

int ftruncate64 (int fd, off64_t length)
{
    touchFd (fd);
    static const auto fn = real<int (*) (int, off64_t)> ("ftruncate64");
    return fn (fd, length);
}

off_t lseek (int fd, off_t off, int whence)
{
    touchFd (fd);
    static const auto fn = real<off_t (*) (int, off_t, int)> ("lseek");
    return fn (fd, off, whence);
}

off64_t lseek64 (int fd, off64_t off, int whence)
{
    touchFd (fd);
    static const auto fn = real<off64_t (*) (int, off64_t, int)> ("lseek64");
    return fn (fd, off, whence);
}

int fstat (int fd, struct stat* st)
{
    touchFd (fd);
    static const auto fn = real<int (*) (int, struct stat*)> ("fstat");
    return fn (fd, st);
}

int fstat64 (int fd, struct stat64* st)
{
    touchFd (fd);
    static const auto fn = real<int (*) (int, struct stat64*)> ("fstat64");
    return fn (fd, st);
}

int stat (const char* path, struct stat* st)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct stat*)> ("stat");
    return fn (path, st);
}

int stat64 (const char* path, struct stat64* st)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct stat64*)> ("stat64");
    return fn (path, st);
}

int lstat (const char* path, struct stat* st)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct stat*)> ("lstat");
    return fn (path, st);
}

int lstat64 (const char* path, struct stat64* st)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct stat64*)> ("lstat64");
    return fn (path, st);
}

int fstatat (int dirfd, const char* path, struct stat* st, int flags)
{
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, struct stat*, int)> ("fstatat");
    return fn (dirfd, path, st, flags);
}

int fstatat64 (int dirfd, const char* path, struct stat64* st, int flags)
{
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, struct stat64*, int)> ("fstatat64");
    return fn (dirfd, path, st, flags);
}

int statx (int dirfd, const char* path, int flags, unsigned mask, struct statx* out)
{
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, int, unsigned, struct statx*)> ("statx");
    return fn (dirfd, path, flags, mask, out);
}

int access (const char* path, int mode)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, int)> ("access");
    return fn (path, mode);
}

int faccessat (int dirfd, const char* path, int mode, int flags)
{
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, int, int)> ("faccessat");
    return fn (dirfd, path, mode, flags);
}

int mkdir (const char* path, mode_t mode)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, mode_t)> ("mkdir");
    return fn (path, mode);
}

int mkdirat (int dirfd, const char* path, mode_t mode)
{
    touchPath (path);
    static const auto fn = real<int (*) (int, const char*, mode_t)> ("mkdirat");
    return fn (dirfd, path, mode);
}

DIR* opendir (const char* path)
{
    touchPath (path);
    static const auto fn = real<DIR* (*) (const char*)> ("opendir");
    return fn (path);
}

int rename (const char* from, const char* to)
{
    static const auto fn = real<int (*) (const char*, const char*)> ("rename");

    if (isTheRenameToHold (to))
    {
        mark (".caught");
        waitWhileCardIsDead();
        const int result = fn (from, to);
        const int savedErrno = errno;
        mark (".released");
        // The trigger is gone by now: the next rename held is one made after
        // it is made again.
        renameHeld.store (false);
        errno = savedErrno;
        return result;
    }

    touchPath (from);
    touchPath (to);
    return fn (from, to);
}

int unlink (const char* path)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*)> ("unlink");
    return fn (path);
}

int rmdir (const char* path)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*)> ("rmdir");
    return fn (path);
}

int statvfs (const char* path, struct statvfs* out)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct statvfs*)> ("statvfs");
    const int r = fn (path, out);
    capComputerFree (r, path, out, r == 0 ? (out->f_frsize != 0 ? out->f_frsize : out->f_bsize) : 0);
    return capCardFree (r, path, out, r == 0 ? (out->f_frsize != 0 ? out->f_frsize : out->f_bsize) : 0);
}

int statvfs64 (const char* path, struct statvfs64* out)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct statvfs64*)> ("statvfs64");
    const int r = fn (path, out);
    capComputerFree (r, path, out, r == 0 ? (out->f_frsize != 0 ? out->f_frsize : out->f_bsize) : 0);
    return capCardFree (r, path, out, r == 0 ? (out->f_frsize != 0 ? out->f_frsize : out->f_bsize) : 0);
}

int statfs (const char* path, struct statfs* out)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct statfs*)> ("statfs");
    const int r = fn (path, out);
    if (r == 0)
        fakeFatCard (path, out);
    capComputerFree (r, path, out, r == 0 ? static_cast<unsigned long> (out->f_bsize) : 0);
    return capCardFree (r, path, out, r == 0 ? static_cast<unsigned long> (out->f_bsize) : 0);
}

int statfs64 (const char* path, struct statfs64* out)
{
    touchPath (path);
    static const auto fn = real<int (*) (const char*, struct statfs64*)> ("statfs64");
    const int r = fn (path, out);
    if (r == 0)
        fakeFatCard (path, out);
    capComputerFree (r, path, out, r == 0 ? static_cast<unsigned long> (out->f_bsize) : 0);
    return capCardFree (r, path, out, r == 0 ? static_cast<unsigned long> (out->f_bsize) : 0);
}

char* realpath (const char* path, char* resolved)
{
    touchPath (path);
    static const auto fn = real<char* (*) (const char*, char*)> ("realpath");
    return fn (path, resolved);
}

} // extern "C"
