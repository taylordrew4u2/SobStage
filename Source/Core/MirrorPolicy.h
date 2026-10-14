#pragma once
#include <cstdint>
#include <string>

namespace mma {

enum class MirrorState
{
    /// Writing a second copy alongside the card.
    Active,
    /// Never started: not enough internal room at arm time (§1, §6.3).
    NotStartedNoSpace,
    /// Turned off in Advanced.
    DisabledByUser,
    /// Was running and was stopped mid-take because internal space ran low.
    /// The card write continues (§6.3): the recording is never interrupted.
    StoppedLowSpace,
    /// Was running and was stopped mid-take because a write to it failed --
    /// the mirror volume was unplugged, went read-only, or died. Distinct from
    /// StoppedLowSpace because the two need different words: one is "your disk
    /// is filling up", the other is "your backup drive is gone". The card
    /// write continues either way.
    StoppedWriteFailed,
    /// Was running and was stopped mid-take because the user unticked the
    /// setting. Distinct from DisabledByUser: this take HAS a copy, it is just
    /// shorter than the card's, and session.json has to say so.
    StoppedByUser,
    /// Never started: the backup would have gone onto the same disk the take
    /// is already being recorded to. That doubles the space a take uses and
    /// protects against nothing -- a disk that fails, fills or is pulled
    /// takes both copies with it -- so it is skipped and the user is told.
    SkippedSameDisk,
    /// Not needed: the take itself is being recorded on this computer and
    /// copied to the card after Stop (see TakeRoute). The take IS the copy on
    /// this computer, and writing it twice to one disk protects nothing.
    NotNeededTakeOnComputer,
};

/// Which disk a path lives on, as far as the system can say. Compared, never
/// shown. Both fields empty means unknown.
struct VolumeIdentity
{
    /// The filesystem device (POSIX st_dev, or a Windows drive root).
    std::string device;
    /// The physical disk behind the volume when that is cleanly knowable
    /// without guessing: on a Mac, the APFS container ("disk3") that every
    /// volume carved from it shares. Empty otherwise.
    std::string physicalDisk;
};

namespace mirrorplacement {

/// True only when both identities are known and say the same disk: the same
/// filesystem device, or two APFS volumes carved from the same container.
/// Unknown is not "same" -- a backup that may be on another disk is kept.
bool sharesDisk (const VolumeIdentity& a, const VolumeIdentity& b) noexcept;

/// The APFS container behind a mounted volume, from statfs's f_fstypename
/// and f_mntfromname ("apfs", "/dev/disk3s5" -> "disk3"; a sealed system
/// snapshot "/dev/disk3s1s1" -> "disk3"). Empty for anything that is not
/// plainly an APFS volume of a local disk.
std::string apfsContainerFromMount (const std::string& fileSystemType, const std::string& mountedFrom);

/// Seconds of recording left in `availableBytes`, or -1 when nothing is being
/// written. A backup kept on the same disk as the take writes there too, so it
/// is counted against the same free space; one on another disk is not.
double remainingSeconds (uint64_t availableBytes, double recordingBytesPerSecond,
                         double backupBytesPerSecond, bool backupSharesDisk) noexcept;

/// What the user is told when the backup is skipped for this reason: the
/// longer form for the activity log, the short one beside the save location
/// and on the saved-take card.
const char* sameDiskExplanation() noexcept;
const char* sameDiskShortNote() noexcept;

} // namespace mirrorplacement

/// §6.3 redundant local mirror. Its whole purpose is turning most card
/// failures from data loss into inconvenience, so the rules about when it runs
/// are worth being exact about.
class MirrorPolicy
{
public:
    /// §6.3 / §1: mirror only if internal free space exceeds 2 GB plus the
    /// projected session size.
    static constexpr int64_t kMinHeadroomBytes = 2LL * 1024 * 1024 * 1024;

    /// §6.3: below 1 GB during a take, stop mirroring and keep the card write
    /// going. Lower than the start threshold on purpose -- stopping is a last
    /// resort, and re-deciding at the same number would flap.
    static constexpr int64_t kStopBytes = 1LL * 1024 * 1024 * 1024;

    /// The setting, for the next take. It never overwrites the state of a take
    /// already under way: a running mirror stays Active (and so stays watched by
    /// the low-space stop) until someone actually stops it -- see
    /// noteStoppedByUser().
    void setEnabledByUser (bool enabled) noexcept;

    /// Decides at arm time whether the mirror starts. `sharesDiskWithRecording`
    /// is mirrorplacement::sharesDisk() for the take's destination and the
    /// backup's folder; when true the mirror is skipped whatever the space.
    MirrorState evaluateAtArm (int64_t internalFreeBytes, int64_t projectedSessionBytes,
                               bool sharesDiskWithRecording = false) noexcept;

    /// Called during a take. Once stopped it never restarts within the same
    /// recording: a mirror with a hole in the middle is not a usable copy.
    MirrorState evaluateDuringRecording (int64_t internalFreeBytes) noexcept;

    MirrorState getState() const noexcept { return state; }

    /// Whether the user wants a mirror, which is not the same question as
    /// whether one is running. The state starts at DisabledByUser and only
    /// becomes Active at arm time, so anything asking "will there be a second
    /// copy of the next take" has to ask this rather than read the state and
    /// conclude the feature is switched off.
    bool isEnabledByUser() const noexcept { return enabledByUser; }
    bool isMirroring() const noexcept { return state == MirrorState::Active; }

    /// True when the mirror stopped mid-take, which §6.3 requires be noted in
    /// session.json.
    bool wasStoppedForSpace() const noexcept { return state == MirrorState::StoppedLowSpace; }

    /// Records that a write to the mirror failed. Like the low-space stop this
    /// is one-way within a take: a copy with a hole in the middle is not a
    /// usable copy, so it never resumes even if the volume comes back.
    ///
    /// Returns true only on the transition, so the caller can say it once
    /// rather than on every poll.
    bool noteWriteFailure() noexcept;

    /// True when the mirror stopped because its destination failed, which §6.3
    /// requires be noted in session.json just as the low-space stop is. Without
    /// it a truncated backup copy is indistinguishable from a complete one.
    bool wasStoppedForWriteFailure() const noexcept { return state == MirrorState::StoppedWriteFailed; }

    /// Records that the user turned the mirror off while it was running. One-way
    /// within a take, like the other stops. Returns true only on the transition
    /// out of Active, so the caller stops the copy and says so exactly once.
    bool noteStoppedByUser() noexcept;

    bool wasStoppedByUser() const noexcept { return state == MirrorState::StoppedByUser; }

    /// True when this take has no backup because it would have shared the
    /// recording's disk.
    bool wasSkippedForSameDisk() const noexcept { return state == MirrorState::SkippedSameDisk; }

    /// Arm time, instead of evaluateAtArm(): this take is recorded on this
    /// computer and copied to the card afterwards, so it has no live backup.
    void noteTakeRecordedOnComputer() noexcept { state = MirrorState::NotNeededTakeOnComputer; }

    void reset() noexcept;

private:
    bool enabledByUser = true;
    MirrorState state = MirrorState::DisabledByUser;
};

} // namespace mma
