#include "MirrorPolicy.h"

#include <cctype>

namespace mma {

namespace mirrorplacement {

bool sharesDisk (const VolumeIdentity& a, const VolumeIdentity& b) noexcept
{
    if (! a.device.empty() && a.device == b.device)
        return true;

    return ! a.physicalDisk.empty() && a.physicalDisk == b.physicalDisk;
}

std::string apfsContainerFromMount (const std::string& fileSystemType, const std::string& mountedFrom)
{
    if (fileSystemType != "apfs")
        return {};

    // "/dev/diskNsM", optionally followed by "sK" for a sealed snapshot.
    // Anything else -- a bare "/dev/diskN", a network or image source -- is
    // not guessed at, and the plain device comparison is all there is.
    const std::string prefix = "/dev/disk";
    if (mountedFrom.compare (0, prefix.size(), prefix) != 0)
        return {};

    size_t i = prefix.size();
    const size_t digitsStart = i;
    while (i < mountedFrom.size() && std::isdigit (static_cast<unsigned char> (mountedFrom[i])) != 0)
        ++i;

    if (i == digitsStart || i + 1 >= mountedFrom.size() || mountedFrom[i] != 's'
        || std::isdigit (static_cast<unsigned char> (mountedFrom[i + 1])) == 0)
        return {};

    return "disk" + mountedFrom.substr (digitsStart, i - digitsStart);
}

double remainingSeconds (uint64_t availableBytes, double recordingBytesPerSecond,
                         double backupBytesPerSecond, bool backupSharesDisk) noexcept
{
    double rate = recordingBytesPerSecond > 0.0 ? recordingBytesPerSecond : 0.0;

    if (backupSharesDisk && backupBytesPerSecond > 0.0)
        rate += backupBytesPerSecond;

    if (rate <= 0.0)
        return -1.0;

    return static_cast<double> (availableBytes) / rate;
}

const char* sameDiskExplanation() noexcept
{
    return "No backup copy: your recordings already go to the same disk the backup would use, so a "
           "second copy there would take twice the space without protecting anything. Record to a "
           "card or an external drive to get a backup on this computer.";
}

const char* sameDiskShortNote() noexcept
{
    return "No backup copy: it would be on the same disk as the recording.";
}

} // namespace mirrorplacement

void MirrorPolicy::setEnabledByUser (bool enabled) noexcept
{
    enabledByUser = enabled;

    // Only a state that describes no copy at all is relabelled. Throwing an
    // Active mirror to DisabledByUser left it writing with nothing watching it:
    // evaluateDuringRecording() only judges an Active mirror, so the 1 GB stop
    // never ran again for the rest of the take.
    if (! enabled && (state == MirrorState::NotStartedNoSpace || state == MirrorState::SkippedSameDisk
                      || state == MirrorState::NotNeededTakeOnComputer))
        state = MirrorState::DisabledByUser;
}

MirrorState MirrorPolicy::evaluateAtArm (int64_t internalFreeBytes, int64_t projectedSessionBytes,
                                         bool sharesDiskWithRecording) noexcept
{
    if (! enabledByUser)
    {
        state = MirrorState::DisabledByUser;
        return state;
    }

    // Before the space test: with both copies on one disk, that test would be
    // judging a copy that should not exist at all.
    if (sharesDiskWithRecording)
    {
        state = MirrorState::SkippedSameDisk;
        return state;
    }

    const int64_t required = kMinHeadroomBytes + projectedSessionBytes;

    state = internalFreeBytes > required ? MirrorState::Active
                                         : MirrorState::NotStartedNoSpace;
    return state;
}

MirrorState MirrorPolicy::evaluateDuringRecording (int64_t internalFreeBytes) noexcept
{
    // Only a running mirror can be stopped. A mirror that never started, or one
    // already stopped, stays as it is: restarting mid-take would leave a hole in
    // the copy and a partial mirror is not a usable one.
    if (state != MirrorState::Active)
        return state;

    if (internalFreeBytes < kStopBytes)
        state = MirrorState::StoppedLowSpace;

    return state;
}

void MirrorPolicy::reset() noexcept
{
    state = enabledByUser ? MirrorState::Active : MirrorState::DisabledByUser;
}

bool MirrorPolicy::noteWriteFailure() noexcept
{
    // Only a mirror that was actually running can stop. A failure reported
    // after it already stopped -- for space, or for an earlier failure -- is
    // not a new event and must not produce a second notice.
    if (state != MirrorState::Active)
        return false;

    state = MirrorState::StoppedWriteFailed;
    return true;
}

bool MirrorPolicy::noteStoppedByUser() noexcept
{
    if (state != MirrorState::Active)
        return false;

    state = MirrorState::StoppedByUser;
    return true;
}

} // namespace mma
