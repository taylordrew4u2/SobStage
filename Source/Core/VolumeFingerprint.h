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

    /// The volume's size, for telling apart two cards without a UUID.
    uint64_t totalBytes = 0;
};

/// True only when both fingerprints are known and describe different
/// volumes. Unknown is never "different": a platform or a moment that cannot
/// say must not throw away a drive check.
///
/// Mounted somewhere else, or a different filesystem, is different. Then, if
/// both volumes have a UUID, the UUIDs decide on their own -- so the same card
/// re-seated in another reader (a new device node) is still the same card.
/// Without one on both sides, the device and the size decide.
inline bool volumesDiffer (const VolumeFingerprint& a, const VolumeFingerprint& b) noexcept
{
    if (! a.known || ! b.known)
        return false;

    if (a.mountRoot != b.mountRoot || a.fsType != b.fsType)
        return true;

    if (! a.uuid.empty() && ! b.uuid.empty())
        return a.uuid != b.uuid;

    return a.device != b.device || a.totalBytes != b.totalBytes;
}

} // namespace mma
