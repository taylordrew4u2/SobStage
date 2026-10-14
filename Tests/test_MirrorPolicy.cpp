#include "TestFramework.h"
#include "Core/MirrorPolicy.h"

using namespace mma;

namespace {
constexpr int64_t kGB = 1024LL * 1024 * 1024;
}

TEST_CASE (MirrorPolicy_MirrorsWhenThereIsRoom)
{
    MirrorPolicy p;
    // 10 GB free, 1 GB session: comfortably past 2 GB plus the session.
    REQUIRE (p.evaluateAtArm (10 * kGB, 1 * kGB) == MirrorState::Active);
    REQUIRE (p.isMirroring());
}

TEST_CASE (MirrorPolicy_RefusesWhenSpaceIsOnlyJustEnough)
{
    MirrorPolicy p;
    // Exactly 2 GB plus the session is not "exceeds" (§6.3).
    REQUIRE (p.evaluateAtArm (MirrorPolicy::kMinHeadroomBytes + 1 * kGB, 1 * kGB)
             == MirrorState::NotStartedNoSpace);
}

TEST_CASE (MirrorPolicy_AccountsForTheProjectedSessionSize)
{
    MirrorPolicy p;
    // 5 GB free is plenty for a small take...
    REQUIRE (p.evaluateAtArm (5 * kGB, 1 * kGB) == MirrorState::Active);

    // ...and not enough for a big one, because the mirror needs room for the
    // whole session, not just the headroom.
    MirrorPolicy q;
    REQUIRE (q.evaluateAtArm (5 * kGB, 4 * kGB) == MirrorState::NotStartedNoSpace);
}

TEST_CASE (MirrorPolicy_StopsBelowOneGigabyteDuringATake)
{
    MirrorPolicy p;
    p.evaluateAtArm (10 * kGB, 1 * kGB);
    REQUIRE (p.isMirroring());

    REQUIRE (p.evaluateDuringRecording (MirrorPolicy::kStopBytes - 1) == MirrorState::StoppedLowSpace);
    REQUIRE_FALSE (p.isMirroring());
    REQUIRE (p.wasStoppedForSpace());
}

TEST_CASE (MirrorPolicy_KeepsMirroringAtExactlyOneGigabyte)
{
    MirrorPolicy p;
    p.evaluateAtArm (10 * kGB, 1 * kGB);
    // §6.3 says below 1 GB, so 1 GB itself still mirrors.
    REQUIRE (p.evaluateDuringRecording (MirrorPolicy::kStopBytes) == MirrorState::Active);
}

TEST_CASE (MirrorPolicy_NeverRestartsWithinTheSameTake)
{
    MirrorPolicy p;
    p.evaluateAtArm (10 * kGB, 1 * kGB);
    p.evaluateDuringRecording (100);
    REQUIRE (p.wasStoppedForSpace());

    // Space freed up again mid-take. Resuming would leave a hole in the copy,
    // and a partial mirror is not a usable one.
    REQUIRE (p.evaluateDuringRecording (50 * kGB) == MirrorState::StoppedLowSpace);
    REQUIRE_FALSE (p.isMirroring());
}

TEST_CASE (MirrorPolicy_AMirrorThatNeverStartedDoesNotStartMidTake)
{
    MirrorPolicy p;
    p.evaluateAtArm (1 * kGB, 1 * kGB);
    REQUIRE (p.getState() == MirrorState::NotStartedNoSpace);

    REQUIRE (p.evaluateDuringRecording (50 * kGB) == MirrorState::NotStartedNoSpace);
}

TEST_CASE (MirrorPolicy_UserDisableWins)
{
    MirrorPolicy p;
    p.setEnabledByUser (false);

    REQUIRE (p.evaluateAtArm (500 * kGB, 1 * kGB) == MirrorState::DisabledByUser);
    REQUIRE_FALSE (p.isMirroring());
}

TEST_CASE (MirrorPolicy_DisablingMidTakeStopsIt)
{
    MirrorPolicy p;
    p.evaluateAtArm (10 * kGB, 1 * kGB);
    REQUIRE (p.isMirroring());

    // The setting alone does not stop the copy being written -- the caller
    // does that, and records it here once. Saying "not mirroring" before the
    // writer had stopped was what took the low-space stop off a live mirror.
    p.setEnabledByUser (false);
    REQUIRE (p.noteStoppedByUser());
    REQUIRE_FALSE (p.noteStoppedByUser());
    REQUIRE_FALSE (p.isMirroring());
    REQUIRE (p.wasStoppedByUser());

    // Never restarted within the take, however much room there is.
    REQUIRE (p.evaluateDuringRecording (500 * kGB) == MirrorState::StoppedByUser);
}

TEST_CASE (MirrorPolicy_StoppedForSpaceIsDistinctFromNeverStarted)
{
    // §6.3 asks for the mid-take stop to be noted in session.json, so the two
    // reasons must not collapse into one flag.
    MirrorPolicy stopped;
    stopped.evaluateAtArm (10 * kGB, 1 * kGB);
    stopped.evaluateDuringRecording (100);
    REQUIRE (stopped.wasStoppedForSpace());

    MirrorPolicy neverStarted;
    neverStarted.evaluateAtArm (1 * kGB, 1 * kGB);
    REQUIRE_FALSE (neverStarted.wasStoppedForSpace());
}

TEST_CASE (MirrorPolicy_ResetPreparesTheNextTake)
{
    MirrorPolicy p;
    p.evaluateAtArm (10 * kGB, 1 * kGB);
    p.evaluateDuringRecording (100);
    REQUIRE (p.wasStoppedForSpace());

    p.reset();
    REQUIRE_FALSE (p.wasStoppedForSpace());
    REQUIRE (p.evaluateAtArm (10 * kGB, 1 * kGB) == MirrorState::Active);
}

TEST_CASE (MirrorPolicy_theSettingIsReadableBeforeTheFirstArm)
{
    MirrorPolicy policy;

    // The state starts at DisabledByUser and only becomes Active at arm time,
    // so a caller asking "will the next take get a second copy" cannot read the
    // state to find out -- before the first arm it would always say no, and the
    // path the user is told about would be missing from the one screen shown
    // before any file exists.
    REQUIRE (policy.getState() == MirrorState::DisabledByUser);
    REQUIRE (policy.isEnabledByUser());

    policy.setEnabledByUser (false);
    REQUIRE_FALSE (policy.isEnabledByUser());

    policy.setEnabledByUser (true);
    REQUIRE (policy.isEnabledByUser());
}

TEST_CASE (MirrorPolicy_AFailedWriteStopsTheMirrorAndIsReportedOnce)
{
    MirrorPolicy p;
    p.setEnabledByUser (true);
    REQUIRE (p.evaluateAtArm (100LL * 1024 * 1024 * 1024, 1024) == MirrorState::Active);

    // The transition is the event: the caller says it once rather than on
    // every poll for the rest of the take.
    REQUIRE (p.noteWriteFailure());
    REQUIRE_FALSE (p.noteWriteFailure());

    REQUIRE (p.getState() == MirrorState::StoppedWriteFailed);
    REQUIRE_FALSE (p.isMirroring());

    // §6.3 requires the stop be visible in session.json, and the two reasons
    // must not be confused for each other.
    REQUIRE (p.wasStoppedForWriteFailure());
    REQUIRE_FALSE (p.wasStoppedForSpace());
}

TEST_CASE (MirrorPolicy_AFailedWriteNeverRestartsTheMirror)
{
    MirrorPolicy p;
    p.setEnabledByUser (true);
    p.evaluateAtArm (100LL * 1024 * 1024 * 1024, 1024);
    REQUIRE (p.noteWriteFailure());

    // Plenty of room, and the volume may even be back -- but a copy with a
    // hole in the middle is not a usable copy, so it stays stopped.
    REQUIRE (p.evaluateDuringRecording (100LL * 1024 * 1024 * 1024)
             == MirrorState::StoppedWriteFailed);
    REQUIRE_FALSE (p.isMirroring());
}

TEST_CASE (MirrorPolicy_AMirrorThatNeverStartedCannotFail)
{
    // A write failure reported against a mirror that was never running is not
    // an event, and must not produce a notice or overwrite why it is not running.
    MirrorPolicy p;
    p.setEnabledByUser (false);
    REQUIRE_FALSE (p.noteWriteFailure());
    REQUIRE (p.getState() == MirrorState::DisabledByUser);

    MirrorPolicy q;
    q.setEnabledByUser (true);
    REQUIRE (q.evaluateAtArm (1024, 100LL * 1024 * 1024 * 1024) == MirrorState::NotStartedNoSpace);
    REQUIRE_FALSE (q.noteWriteFailure());
    REQUIRE (q.getState() == MirrorState::NotStartedNoSpace);
}

TEST_CASE (MirrorPolicy_ALowSpaceStopIsNotRelabelledAsAWriteFailure)
{
    MirrorPolicy p;
    p.setEnabledByUser (true);
    p.evaluateAtArm (100LL * 1024 * 1024 * 1024, 1024);
    REQUIRE (p.evaluateDuringRecording (1024) == MirrorState::StoppedLowSpace);

    // Writes to a stopped mirror can still fail; the reason the user is given
    // must stay the first one, which is the one that actually stopped it.
    REQUIRE_FALSE (p.noteWriteFailure());
    REQUIRE (p.wasStoppedForSpace());
    REQUIRE_FALSE (p.wasStoppedForWriteFailure());
}

TEST_CASE (MirrorPolicy_TheLowSpaceStopStillRunsAfterTheSettingIsUntickedMidTake)
{
    // The tick box is the user's wish for the next take; it does not stop a
    // copy that is already being written. Unticking it mid-take used to throw
    // the state to DisabledByUser while the mirror kept writing, and every
    // later low-space check then saw "not Active" and did nothing -- a backup
    // left to fill the computer's disk with the §6.3 stop switched off.
    MirrorPolicy p;
    REQUIRE (p.evaluateAtArm (10 * kGB, 1 * kGB) == MirrorState::Active);

    p.setEnabledByUser (false);
    REQUIRE_FALSE (p.isEnabledByUser());
    REQUIRE (p.isMirroring());

    REQUIRE (p.evaluateDuringRecording (MirrorPolicy::kStopBytes - 1) == MirrorState::StoppedLowSpace);
    REQUIRE (p.wasStoppedForSpace());

    // And the next take honours the setting.
    p.reset();
    REQUIRE (p.evaluateAtArm (10 * kGB, 1 * kGB) == MirrorState::DisabledByUser);
}

// Recording to the computer's own disk with the backup in ~/RECORDINGS-MIRROR
// on that same disk doubled the space a take used and protected nothing.
TEST_CASE (MirrorPolicy_ABackupOnTheRecordingsOwnDiskIsSkipped)
{
    MirrorPolicy p;
    REQUIRE (p.evaluateAtArm (500 * kGB, 1 * kGB, true) == MirrorState::SkippedSameDisk);
    REQUIRE_FALSE (p.isMirroring());
    REQUIRE (p.wasSkippedForSameDisk());

    // Not a stop: nothing ran, so there is nothing for session.json to call short.
    REQUIRE_FALSE (p.wasStoppedForSpace());
    REQUIRE_FALSE (p.wasStoppedForWriteFailure());
    REQUIRE_FALSE (p.wasStoppedByUser());
    REQUIRE_FALSE (p.noteWriteFailure());
    REQUIRE (p.evaluateDuringRecording (500 * kGB) == MirrorState::SkippedSameDisk);

    // Another disk the next take: the backup runs again.
    p.reset();
    REQUIRE (p.evaluateAtArm (500 * kGB, 1 * kGB, false) == MirrorState::Active);
}

TEST_CASE (MirrorPolicy_TheSameDiskSkipIsJudgedBeforeSpaceAndAfterTheSetting)
{
    // No room AND the same disk: the reason given is the disk. Saying "not
    // enough room" would send the user off to free space for a copy that would
    // still be refused.
    MirrorPolicy cramped;
    REQUIRE (cramped.evaluateAtArm (1 * kGB, 1 * kGB, true) == MirrorState::SkippedSameDisk);

    // Switched off: that is the answer, whatever the disk.
    MirrorPolicy off;
    off.setEnabledByUser (false);
    REQUIRE (off.evaluateAtArm (500 * kGB, 1 * kGB, true) == MirrorState::DisabledByUser);

    // Unticked after a same-disk skip: relabelled, like a never-started one.
    MirrorPolicy skipped;
    skipped.evaluateAtArm (500 * kGB, 1 * kGB, true);
    skipped.setEnabledByUser (false);
    REQUIRE (skipped.getState() == MirrorState::DisabledByUser);
    REQUIRE_FALSE (skipped.wasSkippedForSameDisk());
}

TEST_CASE (MirrorPlacement_SameDeviceIsTheSameDisk)
{
    REQUIRE (mirrorplacement::sharesDisk ({ "dev:16777231", {} }, { "dev:16777231", {} }));
    REQUIRE_FALSE (mirrorplacement::sharesDisk ({ "dev:16777231", {} }, { "dev:16777240", {} }));
}

TEST_CASE (MirrorPlacement_TwoApfsVolumesOfOneContainerAreOneDisk)
{
    // Macintosh HD - Data and a "Recordings" volume added in Disk Utility:
    // different devices, one container, one physical disk.
    REQUIRE (mirrorplacement::sharesDisk ({ "dev:16777231", "disk3" }, { "dev:16777234", "disk3" }));

    // An external APFS drive has its own container.
    REQUIRE_FALSE (mirrorplacement::sharesDisk ({ "dev:16777231", "disk3" }, { "dev:16777250", "disk5" }));
}

TEST_CASE (MirrorPlacement_UnknownIsNeverTheSameDisk)
{
    // A backup that might be on another disk is worth keeping.
    REQUIRE_FALSE (mirrorplacement::sharesDisk ({}, {}));
    REQUIRE_FALSE (mirrorplacement::sharesDisk ({ "dev:1", {} }, {}));
    REQUIRE_FALSE (mirrorplacement::sharesDisk ({}, { "dev:1", {} }));
}

TEST_CASE (MirrorPlacement_ReadsTheApfsContainerOnlyWhenItIsPlain)
{
    using mirrorplacement::apfsContainerFromMount;

    REQUIRE (apfsContainerFromMount ("apfs", "/dev/disk3s5") == "disk3");
    REQUIRE (apfsContainerFromMount ("apfs", "/dev/disk3s1s1") == "disk3"); // sealed system snapshot
    REQUIRE (apfsContainerFromMount ("apfs", "/dev/disk12s2") == "disk12");

    // Not APFS: a card's exFAT volume, an HFS+ drive. st_dev decides those.
    REQUIRE (apfsContainerFromMount ("exfat", "/dev/disk4s1").empty());
    REQUIRE (apfsContainerFromMount ("hfs", "/dev/disk2s2").empty());

    // Nothing that has to be guessed at.
    REQUIRE (apfsContainerFromMount ("apfs", "/dev/disk3").empty());
    REQUIRE (apfsContainerFromMount ("apfs", "/dev/disk3s").empty());
    REQUIRE (apfsContainerFromMount ("apfs", "/dev/disks1").empty());
    REQUIRE (apfsContainerFromMount ("apfs", "//user@server/share").empty());
    REQUIRE (apfsContainerFromMount ("apfs", "").empty());
}

TEST_CASE (MirrorPlacement_ABackupOnTheSameDiskCountsAgainstRemainingTime)
{
    // 3.6 GB at 1 MB/s is an hour; a backup of the same rate on the same disk
    // halves it. One on another disk leaves it alone.
    constexpr uint64_t available = 3600ull * 1000 * 1000;
    REQUIRE_NEAR (mirrorplacement::remainingSeconds (available, 1.0e6, 0.0, true), 3600.0, 1e-9);
    REQUIRE_NEAR (mirrorplacement::remainingSeconds (available, 1.0e6, 1.0e6, true), 1800.0, 1e-9);
    REQUIRE_NEAR (mirrorplacement::remainingSeconds (available, 1.0e6, 1.0e6, false), 3600.0, 1e-9);

    // Nothing being written has no remaining time to give.
    REQUIRE (mirrorplacement::remainingSeconds (available, 0.0, 0.0, true) < 0.0);
}

// A take recorded on this computer and copied to the card afterwards IS the
// copy on this computer. It has no live backup, and must not be told it was
// skipped for sharing a disk -- that sentence is about a different setup.
TEST_CASE (MirrorPolicy_ATakeRecordedOnThisComputerNeedsNoBackup)
{
    MirrorPolicy p;
    p.noteTakeRecordedOnComputer();

    REQUIRE (p.getState() == MirrorState::NotNeededTakeOnComputer);
    REQUIRE_FALSE (p.isMirroring());
    REQUIRE_FALSE (p.wasSkippedForSameDisk());
    REQUIRE_FALSE (p.wasStoppedForSpace());
    REQUIRE_FALSE (p.noteWriteFailure());
    REQUIRE (p.evaluateDuringRecording (10 * kGB) == MirrorState::NotNeededTakeOnComputer);

    // The next take decides again.
    p.reset();
    REQUIRE (p.getState() == MirrorState::Active);
    REQUIRE (p.evaluateAtArm (500 * kGB, 1 * kGB) == MirrorState::Active);

    MirrorPolicy unticked;
    unticked.noteTakeRecordedOnComputer();
    unticked.setEnabledByUser (false);
    REQUIRE (unticked.getState() == MirrorState::DisabledByUser);
}
