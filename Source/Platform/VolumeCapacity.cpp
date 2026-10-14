#include "VolumeCapacity.h"
#include "../Core/Utf8Path.h"

#include <cctype>
#include <filesystem>
#include <system_error>
#include <vector>

#if ! defined(_WIN32)
 #include <cerrno>
 #include <sys/stat.h>
#else
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <cstdio>
#endif

#if defined(__APPLE__)
 #include <sys/mount.h>
 #include <sys/param.h>
#endif

#if defined(__linux__)
 #include <sys/vfs.h>
#endif

#if defined(__APPLE__)
 // Plain C API: no Objective-C needed, so this stays a .cpp file. mma_core
 // already links CoreFoundation for SleepInhibitor.
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace mma {

namespace volumecapacity {

uint64_t choose (uint64_t basicAvailable, std::optional<int64_t> importantUsage) noexcept
{
    if (importantUsage.has_value() && *importantUsage > 0)
        return static_cast<uint64_t> (*importantUsage);

    return basicAvailable;
}

bool isUnderMissingMountPoint (const std::string& path,
                               const std::function<bool (const std::string&)>& exists)
{
    if (path.empty() || path.front() != '/')
        return false;

    std::vector<std::string> parts;
    std::string part;

    for (size_t i = 1; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == '/')
        {
            if (! part.empty())
                parts.push_back (part);
            part.clear();
        }
        else
        {
            part += path[i];
        }
    }

    // How many leading parts name the mount point itself.
    size_t depth = 0;

    if (parts.size() >= 2 && parts[0] == "Volumes")
        depth = 2;
    else if (parts.size() >= 3 && parts[0] == "media")
        depth = 3;
    else if (parts.size() >= 4 && parts[0] == "run" && parts[1] == "media")
        depth = 4;
    else
        return false;

    std::string mountPoint;
    for (size_t i = 0; i < depth; ++i)
        mountPoint += "/" + parts[i];

    return ! exists (mountPoint);
}

} // namespace volumecapacity

namespace {

#if defined(__APPLE__)
std::optional<int64_t> importantUsageCapacity (const std::string& path) noexcept
{
    const CFURLRef url = CFURLCreateFromFileSystemRepresentation (
        kCFAllocatorDefault, reinterpret_cast<const UInt8*> (path.c_str()),
        static_cast<CFIndex> (path.size()), false);

    if (url == nullptr)
        return std::nullopt;

    CFTypeRef value = nullptr;
    const Boolean found = CFURLCopyResourcePropertyForKey (
        url, kCFURLVolumeAvailableCapacityForImportantUsageKey, &value, nullptr);
    CFRelease (url);

    std::optional<int64_t> result;

    if (found && value != nullptr && CFGetTypeID (value) == CFNumberGetTypeID())
    {
        SInt64 bytes = 0;
        if (CFNumberGetValue (static_cast<CFNumberRef> (value), kCFNumberSInt64Type, &bytes))
            result = static_cast<int64_t> (bytes);
    }

    if (value != nullptr)
        CFRelease (value);

    return result;
}

// The volume's UUID as macOS gives it, or empty. FAT and ExFAT volumes have
// one made from their serial number; a volume with none answers nothing.
// Not noexcept: the string it returns can throw, and its caller catches.
std::string volumeUuidString (const std::string& path)
{
    const CFURLRef url = CFURLCreateFromFileSystemRepresentation (
        kCFAllocatorDefault, reinterpret_cast<const UInt8*> (path.c_str()),
        static_cast<CFIndex> (path.size()), false);

    if (url == nullptr)
        return {};

    CFTypeRef value = nullptr;
    const Boolean found = CFURLCopyResourcePropertyForKey (
        url, kCFURLVolumeUUIDStringKey, &value, nullptr);
    CFRelease (url);

    char buffer[128] = {};
    bool copied = false;

    if (found && value != nullptr && CFGetTypeID (value) == CFStringGetTypeID())
        copied = CFStringGetCString (static_cast<CFStringRef> (value), buffer,
                                     static_cast<CFIndex> (sizeof (buffer)), kCFStringEncodingUTF8) != 0;

    if (value != nullptr)
        CFRelease (value);

    return copied ? std::string (buffer) : std::string();
}
#endif

} // namespace

std::optional<uint64_t> availableBytesForRecording (const std::string& path) noexcept
{
    try
    {
        std::error_code error;
        const auto space = std::filesystem::space (pathFromUtf8 (path), error);

        if (error)
            return std::nullopt;

        const auto basic = static_cast<uint64_t> (space.available);

       #if defined(__APPLE__)
        return volumecapacity::choose (basic, importantUsageCapacity (path));
       #else
        return basic;
       #endif
    }
    catch (...)
    {
        return std::nullopt;
    }
}

VolumeIdentity volumeIdentityOf (const std::string& path) noexcept
{
    VolumeIdentity identity;

    try
    {
        std::error_code error;
        auto p = std::filesystem::absolute (pathFromUtf8 (path), error);
        if (error || p.empty())
            return identity;

       #if defined(_WIN32)
        // A drive letter is a volume. Nothing finer is asked: Windows cannot
        // record to a folder that is not on some drive root anyway.
        auto root = utf8FromPath (p.root_name());
        for (auto& c : root)
            c = static_cast<char> (std::toupper (static_cast<unsigned char> (c)));
        if (! root.empty())
            identity.device = "root:" + root;
       #else
        // A card that is not plugged in is not the startup disk its mount
        // point's parent (/Volumes, /media/<user>) is on. Climbing there
        // called the card "on this computer", and a take meant for it went
        // "straight" into a folder that could not be made.
        if (volumecapacity::isUnderMissingMountPoint (utf8FromPath (p), [] (const std::string& candidate)
            {
                struct stat info {};
                return ::stat (candidate.c_str(), &info) == 0;
            }))
            return identity;

        // The backup folder does not exist before its first take: walk up to
        // the nearest folder that does, which is where it will be created.
        for (int depth = 0; depth < 128 && ! p.empty(); ++depth)
        {
            struct stat info {};
            if (::stat (p.c_str(), &info) == 0)
            {
                identity.device = "dev:" + std::to_string (static_cast<unsigned long long> (info.st_dev));

               #if defined(__APPLE__)
                struct statfs fsInfo {};
                if (::statfs (p.c_str(), &fsInfo) == 0)
                    identity.physicalDisk = mirrorplacement::apfsContainerFromMount (fsInfo.f_fstypename,
                                                                                    fsInfo.f_mntfromname);
               #endif
                return identity;
            }

            // Only "not there yet" is worth climbing past. A permission or
            // I/O error says nothing about which disk this is.
            if (errno != ENOENT && errno != ENOTDIR)
                return identity;

            const auto parent = p.parent_path();
            if (parent == p)
                break;
            p = parent;
        }
       #endif
    }
    catch (...)
    {
        return {};
    }

    return identity;
}

VolumeFingerprint volumeFingerprintOf (const std::string& path) noexcept
{
    VolumeFingerprint fingerprint;

    try
    {
       #if defined(__APPLE__) || defined(__linux__)
        std::error_code error;
        auto p = std::filesystem::absolute (pathFromUtf8 (path), error);
        if (error || p.empty())
            return fingerprint;

        // The nearest folder that is there. With the card out its mount point
        // has gone too, and what answers is the disk /Volumes lives on --
        // which is exactly what makes a missing card a different volume.
        struct stat info {};
        bool found = false;

        for (int depth = 0; depth < 128 && ! p.empty(); ++depth)
        {
            if (::stat (p.c_str(), &info) == 0)
            {
                found = true;
                break;
            }

            // Only "not there" is worth climbing past. A permission or I/O
            // error says nothing about which volume this is.
            if (errno != ENOENT && errno != ENOTDIR)
                return fingerprint;

            const auto parent = p.parent_path();
            if (parent == p)
                break;
            p = parent;
        }

        if (! found)
            return fingerprint;

        struct statfs fsInfo {};
        if (::statfs (p.c_str(), &fsInfo) != 0)
            return fingerprint;

        fingerprint.totalBytes = static_cast<uint64_t> (fsInfo.f_blocks)
                               * static_cast<uint64_t> (fsInfo.f_bsize);

        #if defined(__APPLE__)
        fingerprint.mountRoot = fsInfo.f_mntonname;
        fingerprint.fsType = fsInfo.f_fstypename;
        fingerprint.device = fsInfo.f_mntfromname;
        fingerprint.uuid = volumeUuidString (p.native());
        #else
        // Linux has no mount point in statfs: it is the highest folder still
        // on this device.
        auto root = p;
        for (int depth = 0; depth < 128; ++depth)
        {
            const auto parent = root.parent_path();
            if (parent.empty() || parent == root)
                break;

            struct stat parentInfo {};
            if (::stat (parent.c_str(), &parentInfo) != 0 || parentInfo.st_dev != info.st_dev)
                break;

            root = parent;
        }

        fingerprint.mountRoot = utf8FromPath (root);
        fingerprint.fsType = std::to_string (static_cast<unsigned long long> (fsInfo.f_type));
        fingerprint.device = "dev:" + std::to_string (static_cast<unsigned long long> (info.st_dev));
        #endif

        fingerprint.known = true;
       #else
        // Windows: a drive letter is all there is to go on, and it is the
        // path itself, so it cannot tell one card from another. Unknown.
        (void) path;
       #endif
    }
    catch (...)
    {
        return {};
    }

    return fingerprint;
}

std::string cardIdentityOf (const std::string& path) noexcept
{
    try
    {
       #if defined(_WIN32)
        // The volume serial number: set when the card is formatted, and read
        // from whichever card is in the drive now, whatever its letter.
        wchar_t volume[MAX_PATH + 1] = {};
        DWORD serial = 0;
        const auto wide = pathFromUtf8 (path).wstring();

        if (path.empty() || GetVolumePathNameW (wide.c_str(), volume, MAX_PATH) == 0
            || GetVolumeInformationW (volume, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0) == 0)
            return {};

        char text[32] = {};
        std::snprintf (text, sizeof (text), "serial:%08lX", static_cast<unsigned long> (serial));
        return text;
       #else
        // A card that is out: its path climbs to the startup disk, which is
        // never the card.
        if (path.empty() || volumecapacity::isUnderMissingMountPoint (path, [] (const std::string& candidate)
            {
                struct stat info {};
                return ::stat (candidate.c_str(), &info) == 0;
            }))
            return {};

        return cardIdentity (volumeFingerprintOf (path));
       #endif
    }
    catch (...)
    {
        return {};
    }
}

} // namespace mma
