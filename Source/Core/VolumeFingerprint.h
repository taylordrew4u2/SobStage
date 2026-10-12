#pragma once

#include <cstdint>
#include <string>

namespace mma {

/// Which volume is mounted where a save location points, as far as the system
/// can say. Compared, never shown.
///
/// A path is not a drive. macOS mounts every card at /Volumes/<its label>, so
/// a second card with the same label -- two cards formatted in the same
/// camera, or Disk Utility's "Untitled" -- arrives at exactly the path the
/// first one left, and /Volumes is case-insensitive besides. The §6.4 drive
/// check measured one card and was kept for the path, so its speed and its
/// FAT32 verdict armed whatever card was put in next.
struct VolumeFingerprint
{
    /// False when the system could not be asked, or this platform does not
    /// say (Windows). An unknown fingerprint is never compared.
    bool known = false;

    /// Where the volume holding the path -- or, when the path is not there
    /// yet, its nearest existing parent -- is mounted. With the card out that
    /// is the startup disk's, which is what tells "gone" from "still here".
    std::string mountRoot;

    /// The filesystem's type ("msdos", "exfat", "apfs"; a type number on Linux).
    std::string fsType;

    /// The device behind the mount: f_mntfromname on a Mac ("/dev/disk4s1"),
    /// "dev:<st_dev>" elsewhere. A new card in the same reader can be given
    /// the same one, so this alone proves nothing.
    std::string device;

    /// The volume's own UUID, empty when it has none or the system won't say.
    /// FAT and ExFAT cards get one from their serial number on a Mac.
    std::string uuid;

    /// The volume's size, for telling apart two cards without a UUID. Only
    /// weighed for a format whose size is fixed when it is formatted (see
    /// sizeIdentifiesVolume).
    uint64_t totalBytes = 0;
};

/// True when a filesystem of this type (VolumeFingerprint::fsType) reports a
/// size that is set when the volume is formatted, so two volumes reporting
/// different sizes are two volumes. FAT and ExFAT -- the card formats -- are.
///
/// Elsewhere the reported size moves while the same volume stays mounted. A
/// ZFS dataset reports what it holds plus what its pool has free, so every
/// write to any dataset in the pool changes it (on Ubuntu with ZFS on root,
/// the system writes to one dataset while ~/RECORDINGS sits in another). A
/// network share or a FUSE mount reports whatever its server or program says,
/// and a file server on ZFS says exactly that. Weighing those sizes made one
/// mounted drive look like a new one on every status tick, and each time its
/// check was thrown away and run again.
///
/// Mac: statfs's f_fstypename. Linux: the f_type number in decimal --
/// MSDOS_SUPER_MAGIC (0x4d44, vfat too) and EXFAT_SUPER_MAGIC (0x2011BAB0).
inline bool sizeIdentifiesVolume (const std::string& fsType) noexcept
{
    return fsType == "msdos" || fsType == "exfat"
        || fsType == "19780"        // 0x4d44
        || fsType == "538032816";   // 0x2011BAB0
}

/// True only when both fingerprints are known and describe different
/// volumes. Unknown is never "different": a platform or a moment that cannot
/// say must not throw away a drive check.
///
/// Mounted somewhere else, or a different filesystem, is different. Then, if
/// both volumes have a UUID, the UUIDs decide on their own -- so the same card
/// re-seated in another reader (a new device node) is still the same card.
/// Without one on both sides, the device decides, and the size too where the
/// format fixes it (sizeIdentifiesVolume). A drive whose reported size drifts
/// stays the same drive while its mount point, type and device hold; taking
/// it out still shows, because with it gone the mount point is the startup
/// disk's.
inline bool volumesDiffer (const VolumeFingerprint& a, const VolumeFingerprint& b) noexcept
{
    if (! a.known || ! b.known)
        return false;

    if (a.mountRoot != b.mountRoot || a.fsType != b.fsType)
        return true;

    if (! a.uuid.empty() && ! b.uuid.empty())
        return a.uuid != b.uuid;

    if (a.device != b.device)
        return true;

    return sizeIdentifiesVolume (a.fsType) && a.totalBytes != b.totalBytes;
}

} // namespace mma
