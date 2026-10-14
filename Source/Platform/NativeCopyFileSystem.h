#pragma once

#include "../Core/TakeCopy.h"

namespace mma {

namespace cardcopy {

/// What an errno from a card call means for the copy.
FsError classifyPosixError (int errnoValue) noexcept;

/// The same for a Windows error code (GetLastError()), by number, so it can be
/// checked on any platform.
FsError classifyWindowsError (unsigned long windowsError) noexcept;

/// classifyPosixError or classifyWindowsError, for the platform this runs on.
FsError classifyOsError (int osError) noexcept;

} // namespace cardcopy

/// The real disk, for cardcopy::copyTake(). Plain libc calls on a Mac and
/// Linux, and wide Win32 calls on Windows -- nothing buffered or cached in
/// between, so every call that reaches the card is one the test shim
/// (Tools/fs_stall_shim.cpp) can hold, and so a read-back past the cache
/// reads what the card really holds.
///
/// Every call can block for as long as a card that has stopped answering
/// does. Only ever used on CardCopier's worker.
class NativeCopyFileSystem : public cardcopy::CopyFileSystem
{
public:
    bool exists (const std::string& path) override;
    cardcopy::FsResult list (const std::string& folder, std::vector<cardcopy::Entry>& entries) override;
    cardcopy::FsResult makeFolder (const std::string& path) override;
    std::optional<uint64_t> freeBytes (const std::string& path) override;
    std::optional<bool> limitedTo4GiBFiles (const std::string& path) override;
    std::string filesystemTypeName (const std::string& path) override;
    cardcopy::FsResult readText (const std::string& path, std::string& text) override;
    cardcopy::FsResult replaceText (const std::string& path, const std::string& text) override;
    std::unique_ptr<Reader> openRead (const std::string& path, bool bypassCache,
                                      cardcopy::FsResult& result) override;
    std::unique_ptr<Writer> createNew (const std::string& path, cardcopy::FsResult& result) override;
    cardcopy::FsResult rename (const std::string& from, const std::string& to, bool replace) override;
    cardcopy::FsResult remove (const std::string& path) override;
    cardcopy::FsResult syncFolder (const std::string& path) override;
};

} // namespace mma
