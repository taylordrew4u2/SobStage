#include "TestFramework.h"
#include "Platform/VolumeCapacity.h"

#include <filesystem>
#include <set>
#include <string>

using namespace mma;

// APFS's plain free figure leaves out purgeable space macOS frees the moment a
// write needs it, so a Mac showing 50 GB available in Finder could be told its
// drive was full. The "important usage" figure counts that space and wins
// whenever the system gives one.
TEST_CASE (VolumeCapacity_PurgeableSpaceCountsWhenTheSystemReportsIt)
{
    constexpr uint64_t basic = 300ull * 1000 * 1000;
    constexpr int64_t important = 50ll * 1000 * 1000 * 1000;

    REQUIRE (volumecapacity::choose (basic, important) == static_cast<uint64_t> (important));
}

TEST_CASE (VolumeCapacity_FallsBackWhenTheImportantUsageFigureIsMissingOrZero)
{
    constexpr uint64_t basic = 300ull * 1000 * 1000;

    REQUIRE (volumecapacity::choose (basic, std::nullopt) == basic);
    REQUIRE (volumecapacity::choose (basic, int64_t { 0 }) == basic);
    REQUIRE (volumecapacity::choose (basic, int64_t { -1 }) == basic);
}

TEST_CASE (VolumeCapacity_OffTheMacItIsExactlyTheFilesystemFigure)
{
    const auto here = std::filesystem::temp_directory_path();
    const auto available = availableBytesForRecording (here.string());
    REQUIRE (available.has_value());

#if ! defined (__APPLE__)
    // Free space moves under a running system; the two reads are moments
    // apart, so allow a little drift rather than demanding identity.
    const auto direct = static_cast<uint64_t> (std::filesystem::space (here).available);
    const auto drift = *available > direct ? *available - direct : direct - *available;
    REQUIRE (drift < 64ull * 1024 * 1024);
#endif

    REQUIRE_FALSE (availableBytesForRecording (
        (here / "sobstage-no-such-folder" / "deeper").string()).has_value());
}

// The backup folder is judged before it exists, so a path that is not there
// yet must answer for the folder it will be made in.
TEST_CASE (VolumeCapacity_AFolderNotYetMadeIsOnItsParentsDisk)
{
    const auto here = std::filesystem::temp_directory_path();
    const auto existing = volumeIdentityOf (here.string());
    const auto notYet = volumeIdentityOf ((here / "sobstage-not-made-yet" / "RECORDINGS-MIRROR").string());

    REQUIRE_FALSE (existing.device.empty());
    REQUIRE (notYet.device == existing.device);
    REQUIRE (mirrorplacement::sharesDisk (existing, notYet));
}

#if defined (__linux__)
// /proc is its own filesystem on every Linux, so it is a dependable "some
// other volume" for the negative case.
TEST_CASE (VolumeCapacity_TwoFilesystemsAreTwoDisks)
{
    const auto temp = volumeIdentityOf (std::filesystem::temp_directory_path().string());
    const auto proc = volumeIdentityOf ("/proc");

    REQUIRE_FALSE (proc.device.empty());
    REQUIRE_FALSE (mirrorplacement::sharesDisk (temp, proc));
}
#endif

namespace {

VolumeFingerprint card (const char* uuid, const char* device = "/dev/disk4s1",
                        uint64_t size = 64ull * 1000 * 1000 * 1000)
{
    VolumeFingerprint f;
    f.known = true;
    f.mountRoot = "/Volumes/NO NAME";
    f.fsType = "exfat";
    f.device = device;
    f.uuid = uuid;
    f.totalBytes = size;
    return f;
}

} // namespace

// Two cards with the same label land at the same /Volumes path. What tells them
// apart is the volume itself, never the path.
TEST_CASE (VolumeFingerprint_ASecondCardAtTheSamePathIsADifferentVolume)
{
    const auto first = card ("6A1F-0C22");

    REQUIRE_FALSE (volumesDiffer (first, first));

    // Formatted in the same camera, same label, different serial.
    REQUIRE (volumesDiffer (first, card ("51B0-9E14")));

    // The same card back in another reader: a new device node, the same UUID.
    REQUIRE_FALSE (volumesDiffer (first, card ("6A1F-0C22", "/dev/disk6s1")));

    // FAT32 instead of ExFAT at the same path is a different volume whatever
    // else matches -- it is the 4 GiB limit the old verdict knew nothing of.
    auto fat = first;
    fat.fsType = "msdos";
    REQUIRE (volumesDiffer (first, fat));

    // Taken out: what answers at the path is the startup disk.
    auto gone = first;
    gone.mountRoot = "/System/Volumes/Data";
    gone.fsType = "apfs";
    gone.device = "/dev/disk3s5";
    gone.uuid = "A9F7E3C0-1D2B-4C5E-8F60-0123456789AB";
    REQUIRE (volumesDiffer (first, gone));
}

TEST_CASE (VolumeFingerprint_WithoutAUuidTheDeviceAndSizeDecide)
{
    const auto noUuid = card ("");

    REQUIRE_FALSE (volumesDiffer (noUuid, noUuid));
    REQUIRE (volumesDiffer (noUuid, card ("", "/dev/disk5s1")));
    REQUIRE (volumesDiffer (noUuid, card ("", "/dev/disk4s1", 32ull * 1000 * 1000 * 1000)));

    // A card's size is fixed when it is formatted, so even a few megabytes
    // apart -- two "64 GB" cards from different makers -- is two cards.
    REQUIRE (volumesDiffer (noUuid, card ("", "/dev/disk4s1", 64ull * 1000 * 1000 * 1000 - 4096ull * 1024)));

    // The same for FAT32, here as Linux names it (MSDOS_SUPER_MAGIC).
    auto fatOnLinux = noUuid;
    fatOnLinux.mountRoot = "/media/me/NO NAME";
    fatOnLinux.fsType = "19780";
    fatOnLinux.device = "dev:2065";
    auto smallerFat = fatOnLinux;
    smallerFat.totalBytes = 32ull * 1000 * 1000 * 1000;
    REQUIRE (volumesDiffer (fatOnLinux, smallerFat));

    // One side without a UUID (the system declined to say this once) is
    // judged on what both sides have, so it can't make the same card look new.
    REQUIRE_FALSE (volumesDiffer (noUuid, card ("6A1F-0C22")));
}

TEST_CASE (VolumeFingerprint_OnlyCardFormatsAreToldApartBySize)
{
    REQUIRE (sizeIdentifiesVolume ("msdos"));
    REQUIRE (sizeIdentifiesVolume ("exfat"));
    REQUIRE (sizeIdentifiesVolume ("19780"));       // Linux vfat/msdos
    REQUIRE (sizeIdentifiesVolume ("538032816"));   // Linux exfat

    REQUIRE_FALSE (sizeIdentifiesVolume ("smbfs"));
    REQUIRE_FALSE (sizeIdentifiesVolume ("nfs"));
    REQUIRE_FALSE (sizeIdentifiesVolume ("apfs"));
    REQUIRE_FALSE (sizeIdentifiesVolume ("801189825")); // Linux ZFS
    REQUIRE_FALSE (sizeIdentifiesVolume (""));
}

// A ZFS dataset's reported size is what it holds plus what the pool has free,
// so any write anywhere in the pool changes it; a network share passes on
// whatever its server reports. The same mounted drive, a little bigger or
// smaller between two looks, is the same drive -- otherwise every status tick
// threw its check away and started another.
TEST_CASE (VolumeFingerprint_ASizeThatDriftsIsTheSameVolume)
{
    VolumeFingerprint zfsHome;
    zfsHome.known = true;
    zfsHome.mountRoot = "/home";
    zfsHome.fsType = "801189825";   // 0x2fc12fc1, ZFS_SUPER_MAGIC
    zfsHome.device = "dev:44";
    zfsHome.totalBytes = 912ull * 1000 * 1000 * 1000;

    auto afterSystemWrites = zfsHome;
    afterSystemWrites.totalBytes -= 37ull * 1000 * 1000;
    REQUIRE_FALSE (volumesDiffer (zfsHome, afterSystemWrites));

    // A large move too -- a backup into another dataset of the pool.
    auto afterBackup = zfsHome;
    afterBackup.totalBytes -= 80ull * 1000 * 1000 * 1000;
    REQUIRE_FALSE (volumesDiffer (zfsHome, afterBackup));

    // A share on a Mac has no volume UUID; the NAS behind it is often ZFS.
    VolumeFingerprint share;
    share.known = true;
    share.mountRoot = "/Volumes/Recordings";
    share.fsType = "smbfs";
    share.device = "//me@nas._smb._tcp.local/Recordings";
    share.totalBytes = 7800ull * 1000 * 1000 * 1000;

    auto shareLater = share;
    shareLater.totalBytes += 200ull * 1000 * 1000;
    REQUIRE_FALSE (volumesDiffer (share, shareLater));

    // What still tells it apart: another server's share at the same name ...
    auto otherNas = shareLater;
    otherNas.device = "//me@other-nas._smb._tcp.local/Recordings";
    REQUIRE (volumesDiffer (share, otherNas));

    // ... and the share gone, so the startup disk answers at the path.
    auto gone = share;
    gone.mountRoot = "/System/Volumes/Data";
    gone.fsType = "apfs";
    gone.device = "/dev/disk3s5";
    gone.uuid = "A9F7E3C0-1D2B-4C5E-8F60-0123456789AB";
    REQUIRE (volumesDiffer (share, gone));
}

TEST_CASE (VolumeFingerprint_UnknownIsNeverADifferentVolume)
{
    VolumeFingerprint unknown;
    REQUIRE_FALSE (volumesDiffer (unknown, card ("6A1F-0C22")));
    REQUIRE_FALSE (volumesDiffer (card ("6A1F-0C22"), unknown));
    REQUIRE_FALSE (volumesDiffer (unknown, unknown));
}

#if defined (__linux__) || defined (__APPLE__)
// A save location not made yet is answered for the folder it will be made in,
// as the drive check does before its first take.
TEST_CASE (VolumeFingerprint_AFolderNotYetMadeIsOnItsParentsVolume)
{
    const auto here = std::filesystem::temp_directory_path();
    const auto existing = volumeFingerprintOf (here.string());
    const auto notYet = volumeFingerprintOf ((here / "sobstage-not-made-yet" / "RECORDINGS").string());

    REQUIRE (existing.known);
    REQUIRE (notYet.known);
    REQUIRE_FALSE (existing.mountRoot.empty());
    REQUIRE_FALSE (volumesDiffer (existing, notYet));
}
#endif

#if defined (__linux__)
TEST_CASE (VolumeFingerprint_AnotherFilesystemIsAnotherVolume)
{
    const auto temp = volumeFingerprintOf (std::filesystem::temp_directory_path().string());
    const auto proc = volumeFingerprintOf ("/proc/self");

    REQUIRE (proc.known);
    REQUIRE (proc.mountRoot == "/proc");
    REQUIRE (volumesDiffer (temp, proc));

    // The type is f_type in decimal (PROC_SUPER_MAGIC, 0x9fa0) -- the form
    // sizeIdentifiesVolume knows Linux's FAT and ExFAT by.
    REQUIRE (proc.fsType == "40864");
}
#endif

// With the card out, its path climbed to /Volumes -- on the startup disk -- so
// a missing card read as "on this computer" and the take went "straight" into
// a folder that could not be made.
TEST_CASE (VolumeCapacity_APathOnAMissingCardIsUnderAMissingMountPoint)
{
    const std::set<std::string> present { "/Volumes", "/Volumes/Macintosh HD", "/media", "/media/sam",
                                          "/media/sam/CARD", "/run", "/run/media", "/run/media/sam" };
    const auto exists = [&present] (const std::string& p) { return present.count (p) > 0; };

    REQUIRE (volumecapacity::isUnderMissingMountPoint ("/Volumes/SD/RECORDINGS", exists));
    REQUIRE (volumecapacity::isUnderMissingMountPoint ("/Volumes/SD", exists));
    REQUIRE (volumecapacity::isUnderMissingMountPoint ("/Volumes/SD/", exists));
    REQUIRE_FALSE (volumecapacity::isUnderMissingMountPoint ("/Volumes/Macintosh HD/Users/sam", exists));
    REQUIRE_FALSE (volumecapacity::isUnderMissingMountPoint ("/Volumes", exists));

    REQUIRE (volumecapacity::isUnderMissingMountPoint ("/media/sam/OTHER/RECORDINGS", exists));
    REQUIRE_FALSE (volumecapacity::isUnderMissingMountPoint ("/media/sam/CARD/RECORDINGS", exists));
    REQUIRE_FALSE (volumecapacity::isUnderMissingMountPoint ("/media/sam", exists));

    REQUIRE (volumecapacity::isUnderMissingMountPoint ("/run/media/sam/CARD/RECORDINGS", exists));
    REQUIRE_FALSE (volumecapacity::isUnderMissingMountPoint ("/run/media/sam", exists));

    // Anywhere else is not a removable mount point, and Windows paths never are.
    REQUIRE_FALSE (volumecapacity::isUnderMissingMountPoint ("/Users/sam/RECORDINGS", exists));
    REQUIRE_FALSE (volumecapacity::isUnderMissingMountPoint ("/home/sam/Volumes/x", exists));
    REQUIRE_FALSE (volumecapacity::isUnderMissingMountPoint ("E:\\RECORDINGS", exists));
    REQUIRE_FALSE (volumecapacity::isUnderMissingMountPoint ("", exists));
}

#if ! defined (_WIN32)
// The card at /Volumes/<name> is out: its path used to climb to /Volumes, on
// the startup disk, and so read as "on this computer". Unknown is the true
// answer -- it is not this computer, as far as anyone can say.
TEST_CASE (VolumeCapacity_AMissingCardIsAnUnknownDiskNotTheStartupDisk)
{
    for (const char* missing : { "/Volumes/SOBSTAGE-NO-SUCH-CARD/RECORDINGS",
                                 "/media/sobstage-no-such-user/CARD/RECORDINGS",
                                 "/run/media/sobstage-no-such-user/CARD/RECORDINGS" })
    {
        const auto identity = volumeIdentityOf (missing);
        REQUIRE (identity.device.empty());
        REQUIRE (identity.physicalDisk.empty());
        REQUIRE_FALSE (mirrorplacement::sharesDisk (identity, volumeIdentityOf ("/")));
    }
}
#endif
