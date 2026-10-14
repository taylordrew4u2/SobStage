#include "NativeCopyFileSystem.h"

#include "VolumeCapacity.h"
#include "../Core/PreflightThroughputTest.h"
#include "../Core/Utf8Path.h"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <dirent.h>
 #include <fcntl.h>
 #include <sys/mman.h>
 #include <sys/stat.h>
 #include <sys/types.h>
 #include <unistd.h>
 #if defined(__APPLE__)
  #include <sys/mount.h>
  #include <sys/param.h>
 #else
  #include <sys/vfs.h>
 #endif
#endif

namespace mma {

namespace cardcopy {

FsError classifyPosixError (int e) noexcept
{
    switch (e)
    {
        case 0:
            return FsError::None;
        case ENOENT:
        case ENODEV:
        case ENXIO:
            return FsError::NotFound;
        case ENOSPC:
       #if defined(EDQUOT)
        case EDQUOT:
       #endif
            return FsError::NoSpace;
        case EROFS:
            return FsError::ReadOnly;
        // EPERM as well as EACCES: macOS refuses an app the user said "Don't
        // Allow" to, for files on a removable volume, with EPERM.
        case EACCES:
        case EPERM:
            return FsError::NotAllowed;
        case EFBIG:
            return FsError::TooBig;
        default:
            return FsError::Other;
    }
}

FsError classifyWindowsError (unsigned long e) noexcept
{
    // The numbers from winerror.h, so this is checked on every platform.
    switch (e)
    {
        case 0:    return FsError::None;
        case 2:    // ERROR_FILE_NOT_FOUND
        case 3:    // ERROR_PATH_NOT_FOUND
        case 15:   // ERROR_INVALID_DRIVE
        case 21:   // ERROR_NOT_READY: a card reader with no card in it
        case 55:   // ERROR_DEV_NOT_EXIST
        case 1167: // ERROR_DEVICE_NOT_CONNECTED
            return FsError::NotFound;
        case 39:   // ERROR_HANDLE_DISK_FULL
        case 112:  // ERROR_DISK_FULL
            return FsError::NoSpace;
        case 19:   // ERROR_WRITE_PROTECT: the card's lock switch
            return FsError::ReadOnly;
        case 5:    // ERROR_ACCESS_DENIED
            return FsError::NotAllowed;
        case 223:  // ERROR_FILE_TOO_LARGE
            return FsError::TooBig;
        default:
            return FsError::Other;
    }
}

FsError classifyOsError (int osError) noexcept
{
   #if defined(_WIN32)
    return classifyWindowsError (static_cast<unsigned long> (osError));
   #else
    return classifyPosixError (osError);
   #endif
}

} // namespace cardcopy

using cardcopy::Entry;
using cardcopy::FsError;
using cardcopy::FsResult;

namespace {

#if defined(_WIN32)

FsResult lastError()
{
    const auto e = GetLastError();
    return { cardcopy::classifyWindowsError (e), static_cast<int> (e) };
}

std::wstring wide (const std::string& utf8)
{
    return pathFromUtf8 (utf8).wstring();
}

// Antivirus and indexing services open a new file for a moment as it is
// closed; a rename in that moment is refused with a sharing violation.
BOOL moveWithRetry (const std::wstring& from, const std::wstring& to, DWORD flags)
{
    for (int attempt = 0;; ++attempt)
    {
        if (MoveFileExW (from.c_str(), to.c_str(), flags | MOVEFILE_WRITE_THROUGH) != 0)
            return TRUE;

        if (GetLastError() != ERROR_SHARING_VIOLATION || attempt >= 3)
            return FALSE;

        Sleep (200);
    }
}

struct WindowsReader : cardcopy::CopyFileSystem::Reader
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool unbuffered = false;
    // FILE_FLAG_NO_BUFFERING reads sector multiples into sector-aligned
    // memory, so an unbuffered reader reads into its own page-aligned block.
    unsigned char* block = nullptr;
    DWORD blockLength = 0, blockPos = 0;

    ~WindowsReader() override
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle (handle);
        if (block != nullptr)
            VirtualFree (block, 0, MEM_RELEASE);
    }

    FsResult read (void* buffer, size_t bytes, size_t& got) override
    {
        got = 0;

        if (! unbuffered)
        {
            DWORD n = 0;
            const auto want = static_cast<DWORD> (bytes > 0x40000000u ? 0x40000000u : bytes);
            if (ReadFile (handle, buffer, want, &n, nullptr) == 0)
                return lastError();
            got = n;
            return {};
        }

        if (blockPos == blockLength)
        {
            blockPos = blockLength = 0;
            if (ReadFile (handle, block, static_cast<DWORD> (cardcopy::kChunkBytes), &blockLength, nullptr) == 0)
                return lastError();
        }

        const auto available = static_cast<size_t> (blockLength - blockPos);
        got = bytes < available ? bytes : available;
        std::memcpy (buffer, block + blockPos, got);
        blockPos += static_cast<DWORD> (got);
        return {};
    }
};

struct WindowsWriter : cardcopy::CopyFileSystem::Writer
{
    HANDLE handle = INVALID_HANDLE_VALUE;

    ~WindowsWriter() override
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle (handle);
    }

    FsResult write (const void* data, size_t bytes) override
    {
        auto* p = static_cast<const unsigned char*> (data);

        while (bytes > 0)
        {
            DWORD n = 0;
            const auto want = static_cast<DWORD> (bytes > 0x40000000u ? 0x40000000u : bytes);
            if (WriteFile (handle, p, want, &n, nullptr) == 0)
                return lastError();
            if (n == 0)
                return { FsError::Other, 0 };
            p += n;
            bytes -= n;
        }

        return {};
    }

    FsResult syncAndClose() override
    {
        FsResult result;
        if (FlushFileBuffers (handle) == 0)
            result = lastError();

        if (CloseHandle (handle) == 0 && result.ok())
            result = lastError();

        handle = INVALID_HANDLE_VALUE;
        return result;
    }
};

#else

FsResult fromErrno (int e)
{
    return { cardcopy::classifyPosixError (e), e };
}

// To the card itself, not only to the drive's own cache: F_FULLFSYNC on a
// Mac, where plain fsync stops at the drive. As SessionWriter does.
bool fullSync (int fd)
{
   #if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (::fcntl (fd, F_FULLFSYNC) == 0)
        return true;
   #endif
    return ::fsync (fd) == 0;
}

#if defined(__APPLE__)
// The file's pages dropped from memory. msync with MS_INVALIDATE on a
// mapping of the whole file is how a Mac lets a process do that; the file
// was flushed before this, so nothing unwritten is lost.
void evictFromMemory (int fd)
{
    struct stat info {};
    if (::fstat (fd, &info) != 0 || info.st_size <= 0)
        return;

    const auto length = static_cast<size_t> (info.st_size);
    void* mapped = ::mmap (nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED)
        return;

    (void) ::msync (mapped, length, MS_INVALIDATE);
    ::munmap (mapped, length);
}
#endif

struct PosixReader : cardcopy::CopyFileSystem::Reader
{
    int fd = -1;

    ~PosixReader() override
    {
        if (fd >= 0)
            ::close (fd);
    }

    FsResult read (void* buffer, size_t bytes, size_t& got) override
    {
        for (;;)
        {
            const auto n = ::read (fd, buffer, bytes);
            if (n >= 0)
            {
                got = static_cast<size_t> (n);
                return {};
            }
            if (errno != EINTR)
            {
                got = 0;
                return fromErrno (errno);
            }
        }
    }
};

struct PosixWriter : cardcopy::CopyFileSystem::Writer
{
    int fd = -1;

    ~PosixWriter() override
    {
        if (fd >= 0)
            ::close (fd);
    }

    FsResult write (const void* data, size_t bytes) override
    {
        auto* p = static_cast<const unsigned char*> (data);

        while (bytes > 0)
        {
            const auto n = ::write (fd, p, bytes);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
               #if defined(__APPLE__)
                // With F_NOCACHE on, a Mac answers a write that crosses the
                // largest file the volume (or the process) allows with
                // ERANGE, not EFBIG: measured on macOS 14, 4 MB written past
                // a 1 MB limit. Read as "too big", so the file stays on this
                // computer and the rest of the take still goes.
                if (errno == ERANGE)
                    return { FsError::TooBig, ERANGE };
               #endif
                return fromErrno (errno);
            }
            if (n == 0)
                return { FsError::Other, EIO };
            p += n;
            bytes -= static_cast<size_t> (n);
        }

        return {};
    }

    FsResult syncAndClose() override
    {
        FsResult result;
        if (! fullSync (fd))
            result = fromErrno (errno);

        // A card that lost a write can say so only at close.
        if (::close (fd) != 0 && result.ok())
            result = fromErrno (errno);

        fd = -1;
        return result;
    }
};

#endif

} // namespace

bool NativeCopyFileSystem::exists (const std::string& path)
{
   #if defined(_WIN32)
    return GetFileAttributesW (wide (path).c_str()) != INVALID_FILE_ATTRIBUTES;
   #else
    struct stat info {};
    return ::stat (path.c_str(), &info) == 0;
   #endif
}

FsResult NativeCopyFileSystem::list (const std::string& folder, std::vector<Entry>& entries)
{
    entries.clear();

   #if defined(_WIN32)
    WIN32_FIND_DATAW found {};
    const auto pattern = wide (cardcopy::joinPath (folder, "*"));
    const HANDLE search = FindFirstFileW (pattern.c_str(), &found);

    if (search == INVALID_HANDLE_VALUE)
        return lastError();

    do
    {
        const auto name = utf8FromPath (std::filesystem::path (found.cFileName));
        if (name == "." || name == "..")
            continue;

        Entry e;
        e.name = name;
        e.isFolder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = (static_cast<uint64_t> (found.nFileSizeHigh) << 32) | found.nFileSizeLow;

        // A link is listed as itself. Looked through, as stat does elsewhere:
        // one that leads nowhere is left out, as there, and one that leads
        // somewhere has its target's size, which is what a copy would read.
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            const HANDLE target = CreateFileW (wide (cardcopy::joinPath (folder, name)).c_str(),
                                               FILE_READ_ATTRIBUTES,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (target == INVALID_HANDLE_VALUE)
                continue;

            BY_HANDLE_FILE_INFORMATION info {};
            const bool known = GetFileInformationByHandle (target, &info) != 0;
            CloseHandle (target);
            if (! known)
                continue;

            e.isFolder = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.size = (static_cast<uint64_t> (info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        }

        entries.push_back (std::move (e));
    }
    while (FindNextFileW (search, &found) != 0);

    const auto ended = GetLastError();
    FindClose (search);
    return ended == ERROR_NO_MORE_FILES ? FsResult {} : FsResult { cardcopy::classifyWindowsError (ended), static_cast<int> (ended) };
   #else
    DIR* dir = ::opendir (folder.c_str());
    if (dir == nullptr)
        return fromErrno (errno);

    int listingError = 0;

    for (;;)
    {
        // errno is read straight after readdir and nowhere else: the stat
        // below, or anything it calls, may leave its own errno behind, and an
        // entry gone between the two is no reason to call the listing failed.
        errno = 0;
        const auto* item = ::readdir (dir);

        if (item == nullptr)
        {
            listingError = errno;
            break;
        }

        const std::string name = item->d_name;
        if (name == "." || name == "..")
            continue;

        // The full path, not the name against the folder's descriptor: a
        // stat of a path on the card is what a card that stops answering
        // holds, and what the test shim can hold.
        struct stat info {};
        if (::stat (cardcopy::joinPath (folder, name).c_str(), &info) != 0)
            continue; // gone since the listing: not part of the take

        Entry e;
        e.name = name;
        e.isFolder = S_ISDIR (info.st_mode);
        e.size = static_cast<uint64_t> (info.st_size);
        entries.push_back (std::move (e));
    }

    ::closedir (dir);
    return listingError == 0 ? FsResult {} : fromErrno (listingError);
   #endif
}

FsResult NativeCopyFileSystem::makeFolder (const std::string& path)
{
   #if defined(_WIN32)
    return CreateDirectoryW (wide (path).c_str(), nullptr) != 0 ? FsResult {} : lastError();
   #else
    return ::mkdir (path.c_str(), 0755) == 0 ? FsResult {} : fromErrno (errno);
   #endif
}

std::optional<uint64_t> NativeCopyFileSystem::freeBytes (const std::string& path)
{
    return availableBytesForRecording (path);
}

std::string NativeCopyFileSystem::filesystemTypeName (const std::string& path)
{
   #if defined(_WIN32)
    wchar_t volume[MAX_PATH + 1] = {};
    wchar_t fsName[MAX_PATH + 1] = {};

    if (GetVolumePathNameW (wide (path).c_str(), volume, MAX_PATH) == 0
        || GetVolumeInformationW (volume, nullptr, 0, nullptr, nullptr, nullptr, fsName, MAX_PATH) == 0)
        return {};

    return utf8FromPath (std::filesystem::path (fsName));
   #else
    // The folder itself, or the one holding it when it has not been made.
    for (auto candidate = path;; candidate = cardcopy::parentOf (candidate))
    {
        struct statfs info {};
        if (::statfs (candidate.c_str(), &info) == 0)
        {
           #if defined(__APPLE__)
            return info.f_fstypename;
           #else
            switch (static_cast<unsigned long> (info.f_type))
            {
                case 0x4d44ul:     return "vfat";  // MSDOS_SUPER_MAGIC
                case 0x2011bab0ul: return "exfat";
                case 0x5346544eul: return "ntfs";
                case 0x7366746eul: return "ntfs";  // ntfs3
                default:           return {};
            }
           #endif
        }

        if (errno != ENOENT || cardcopy::parentOf (candidate) == candidate)
            return {};
    }
   #endif
}

std::optional<bool> NativeCopyFileSystem::limitedTo4GiBFiles (const std::string& path)
{
    const auto name = filesystemTypeName (path);
    if (name.empty())
        return std::nullopt;

    return PreflightThroughputTest::needsReformat (PreflightThroughputTest::filesystemKindFromTypeName (name));
}

FsResult NativeCopyFileSystem::readText (const std::string& path, std::string& text)
{
    text.clear();

   #if defined(_WIN32)
    const HANDLE h = CreateFileW (wide (path).c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return lastError();

    char buffer[65536];
    for (;;)
    {
        DWORD n = 0;
        if (ReadFile (h, buffer, sizeof (buffer), &n, nullptr) == 0)
        {
            const auto failed = lastError();
            CloseHandle (h);
            return failed;
        }
        if (n == 0)
            break;
        text.append (buffer, n);
    }

    CloseHandle (h);
    return {};
   #else
    const int fd = ::open (path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return fromErrno (errno);

    char buffer[65536];
    for (;;)
    {
        const auto n = ::read (fd, buffer, sizeof (buffer));
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            const int e = errno;
            ::close (fd);
            return fromErrno (e);
        }
        if (n == 0)
            break;
        text.append (buffer, static_cast<size_t> (n));
    }

    ::close (fd);
    return {};
   #endif
}

FsResult NativeCopyFileSystem::replaceText (const std::string& path, const std::string& text)
{
    // Hidden, beside the file, so the rename never crosses a disk.
    const auto temp = cardcopy::joinPath (cardcopy::parentOf (path),
                                          "." + cardcopy::lastPartOf (path) + ".sobtmp");

   #if defined(_WIN32)
    const auto wideTemp = wide (temp);
    const HANDLE h = CreateFileW (wideTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return lastError();

    WindowsWriter writer;
    writer.handle = h;
    auto result = writer.write (text.data(), text.size());
    const auto closed = writer.syncAndClose();
    if (result.ok())
        result = closed;

    if (result.ok() && moveWithRetry (wideTemp, wide (path), MOVEFILE_REPLACE_EXISTING) == 0)
        result = lastError();

    if (! result.ok())
        DeleteFileW (wideTemp.c_str());

    return result;
   #else
    const int fd = ::open (temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return fromErrno (errno);

    PosixWriter writer;
    writer.fd = fd;
    auto result = writer.write (text.data(), text.size());
    const auto closed = writer.syncAndClose();
    if (result.ok())
        result = closed;

    if (result.ok() && ::rename (temp.c_str(), path.c_str()) != 0)
        result = fromErrno (errno);

    if (! result.ok())
        ::unlink (temp.c_str());

    return result;
   #endif
}

std::unique_ptr<cardcopy::CopyFileSystem::Reader> NativeCopyFileSystem::openRead (const std::string& path,
                                                                                bool bypassCache,
                                                                                FsResult& result)
{
   #if defined(_WIN32)
    const DWORD flags = FILE_FLAG_SEQUENTIAL_SCAN | (bypassCache ? FILE_FLAG_NO_BUFFERING : 0);
    const HANDLE h = CreateFileW (wide (path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        result = lastError();
        return nullptr;
    }

    auto reader = std::make_unique<WindowsReader>();
    reader->handle = h;
    reader->unbuffered = bypassCache;

    if (bypassCache)
    {
        reader->block = static_cast<unsigned char*> (VirtualAlloc (nullptr, cardcopy::kChunkBytes,
                                                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (reader->block == nullptr)
        {
            result = lastError();
            return nullptr;
        }
    }

    result = {};
    return reader;
   #else
    const int fd = ::open (path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        result = fromErrno (errno);
        return nullptr;
    }

    if (bypassCache)
    {
       #if defined(__APPLE__)
        // F_NOCACHE alone is not enough: a Mac serves an uncached read from
        // any page it still holds, for coherency, and the pages just written
        // are still held after the flush. Those are thrown away first, so the
        // read has to come from the card itself and a card that dropped or
        // garbled the write is caught.
        evictFromMemory (fd);
        (void) ::fcntl (fd, F_NOCACHE, 1);
       #elif defined(POSIX_FADV_DONTNEED)
        // The file was flushed before this, so its pages here are clean and
        // dropping them makes the read come from the card.
        (void) ::posix_fadvise (fd, 0, 0, POSIX_FADV_DONTNEED);
       #endif
    }

    auto reader = std::make_unique<PosixReader>();
    reader->fd = fd;
    result = {};
    return reader;
   #endif
}

std::unique_ptr<cardcopy::CopyFileSystem::Writer> NativeCopyFileSystem::createNew (const std::string& path,
                                                                                 FsResult& result)
{
   #if defined(_WIN32)
    const HANDLE h = CreateFileW (wide (path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        result = lastError();
        return nullptr;
    }

    auto writer = std::make_unique<WindowsWriter>();
    writer->handle = h;
    result = {};
    return writer;
   #else
    const int fd = ::open (path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        result = fromErrno (errno);
        return nullptr;
    }

   #if defined(__APPLE__)
    // Written past this computer's memory, so what the read-back finds is
    // not a copy of this write still held here.
    (void) ::fcntl (fd, F_NOCACHE, 1);
   #endif

    auto writer = std::make_unique<PosixWriter>();
    writer->fd = fd;
    result = {};
    return writer;
   #endif
}

FsResult NativeCopyFileSystem::rename (const std::string& from, const std::string& to, bool replace)
{
    // Checked first rather than with renameat2/renamex_np, whose support on
    // FAT and ExFAT is uneven. This app is the only writer in the take's
    // folder on the card, so nothing can appear in between.
    if (! replace && exists (to))
       #if defined(_WIN32)
        return { FsError::Other, static_cast<int> (ERROR_ALREADY_EXISTS) };
       #else
        return { FsError::Other, EEXIST };
       #endif

   #if defined(_WIN32)
    return moveWithRetry (wide (from), wide (to), replace ? MOVEFILE_REPLACE_EXISTING : 0) != 0
               ? FsResult {} : lastError();
   #else
    return ::rename (from.c_str(), to.c_str()) == 0 ? FsResult {} : fromErrno (errno);
   #endif
}

FsResult NativeCopyFileSystem::remove (const std::string& path)
{
   #if defined(_WIN32)
    return DeleteFileW (wide (path).c_str()) != 0 ? FsResult {} : lastError();
   #else
    return ::unlink (path.c_str()) == 0 ? FsResult {} : fromErrno (errno);
   #endif
}

FsResult NativeCopyFileSystem::syncFolder (const std::string& path)
{
   #if defined(_WIN32)
    // Nothing more to do: every rename here is MOVEFILE_WRITE_THROUGH, which
    // returns only once the new name is on the card.
    (void) path;
    return {};
   #else
    const int fd = ::open (path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return fromErrno (errno);

    FsResult result;
    if (! fullSync (fd))
    {
        const int e = errno;

        // A format that cannot flush a folder on its own says so rather than
        // failing; its renames are as safe as that format can make them.
        if (e != EINVAL && e != ENOTSUP && e != EOPNOTSUPP && e != EBADF)
            result = fromErrno (e);
    }

    ::close (fd);
    return result;
   #endif
}

} // namespace mma
