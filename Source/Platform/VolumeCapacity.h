#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "../Core/MirrorPolicy.h"
#include "../Core/VolumeFingerprint.h"

namespace mma {

namespace volumecapacity {

/// The figure to use from the two the system can give.
///
/// `basicAvailable` is statfs's f_bavail (std::filesystem::space().available).
/// On APFS that leaves out purgeable space -- iCloud "Optimize Mac storage"
/// copies, caches, local snapshots -- which macOS frees the moment a write
/// needs it, so a Mac whose Finder shows 50 GB available can report a few
/// hundred megabytes here. `importantUsage` is the volume's "available for
/// important usage" capacity, which counts that space; it is used when the
/// system gave one (present and non-zero), and the basic figure otherwise.
uint64_t choose (uint64_t basicAvailable, std::optional<int64_t> importantUsage) noexcept;

/// True when `path` (UTF-8) is on a removable volume that is not mounted: a
/// Mac's /Volumes/<X>/..., or Linux's /media/<user>/<X>/... or
/// /run/media/<user>/<X>/..., where that <X> folder is not there.
///
/// With the card out, its path climbs to /Volumes, which is on the startup
/// disk -- so the card read as "on this computer". `exists` is asked about
/// the mount point alone. Windows paths never match: a missing E:\ is
/// already a different drive from C:\.
bool isUnderMissingMountPoint (const std::string& path,
                               const std::function<bool (const std::string&)>& exists);

} // namespace volumecapacity

/// Bytes a recording can still use on the volume holding `path` (an existing
/// file or folder, UTF-8), or empty when the system cannot say.
///
/// macOS: kCFURLVolumeAvailableCapacityForImportantUsageKey, falling back to
/// std::filesystem::space when that is unavailable or zero. Elsewhere exactly
/// std::filesystem::space().available. May block on a disappearing volume the
/// same way std::filesystem::space does, so callers keep it off the message
/// thread wherever they already kept that off it.
std::optional<uint64_t> availableBytesForRecording (const std::string& path) noexcept;

/// Which disk `path` (UTF-8) is on, for mirrorplacement::sharesDisk(). A path
/// that does not exist yet -- the backup folder before its first take -- is
/// answered for its nearest existing parent, which is where it will be made.
///
/// POSIX: st_dev, and on a Mac also the APFS container from statfs, so two
/// volumes of one container count as one disk. Windows: the drive root.
/// Both fields empty when the system cannot say. Like the capacity query this
/// can block on a disappearing volume.
VolumeIdentity volumeIdentityOf (const std::string& path) noexcept;

/// Which volume is mounted where `path` (UTF-8) points, for telling one card
/// from another that took its place (see VolumeFingerprint). A path that is
/// not there is answered for its nearest existing parent -- with the card out,
/// the startup disk -- so a missing card reads as a different volume.
///
/// Mac: statfs's mount point, type and device, the volume UUID and size.
/// Linux: the mount point found by st_dev, the filesystem type number, st_dev
/// and the size. Windows: unknown. Can block on a disappearing volume like
/// the queries above, so it is asked on a worker or under a deadline.
VolumeFingerprint volumeFingerprintOf (const std::string& path) noexcept;

} // namespace mma
