#include "TestFramework.h"
#include <cerrno>
#include "Core/PreflightThroughputTest.h"

using namespace mma;

TEST_CASE (PreflightThroughputTest_RequiredRateFormula)
{
    // 4 channels, 48kHz, 3 bytes/sample (24-bit), x2 overhead.
    double required = PreflightThroughputTest::requiredBytesPerSecond (4, 48000.0, 3);
    REQUIRE_NEAR (required, 4.0 * 48000.0 * 3.0 * 2.0, 1e-6);
}

TEST_CASE (PreflightThroughputTest_SustainedMinimumIsMinNotAverage)
{
    std::vector<double> samples = { 100.0, 50.0, 90.0, 200.0 };
    REQUIRE_NEAR (PreflightThroughputTest::sustainedMinimum (samples), 50.0, 1e-9);
}

TEST_CASE (PreflightThroughputTest_PassesWhenSustainedIsAtLeastTwiceRequired)
{
    double required = PreflightThroughputTest::requiredBytesPerSecond (2, 48000.0, 2);
    std::vector<double> samples = { required * 2.5, required * 3.0 };
    auto result = PreflightThroughputTest::evaluate (samples, 2, 48000.0, 2);
    REQUIRE (result.passed);
}

TEST_CASE (PreflightThroughputTest_BlocksWhenUnderTwiceRequired)
{
    double required = PreflightThroughputTest::requiredBytesPerSecond (2, 48000.0, 2);
    std::vector<double> samples = { required * 1.5 };
    auto result = PreflightThroughputTest::evaluate (samples, 2, 48000.0, 2);
    REQUIRE_FALSE (result.passed);
    REQUIRE_FALSE (result.reason.empty());
}

TEST_CASE (PreflightThroughputTest_BlocksExactlyAtBoundaryJustBelow)
{
    double required = PreflightThroughputTest::requiredBytesPerSecond (1, 48000.0, 2);
    std::vector<double> samples = { required * 1.999 };
    auto result = PreflightThroughputTest::evaluate (samples, 1, 48000.0, 2);
    REQUIRE_FALSE (result.passed);
}

TEST_CASE (PreflightThroughputTest_PassesExactlyAtTwiceRequired)
{
    double required = PreflightThroughputTest::requiredBytesPerSecond (1, 48000.0, 2);
    std::vector<double> samples = { required * 2.0 };
    auto result = PreflightThroughputTest::evaluate (samples, 1, 48000.0, 2);
    REQUIRE (result.passed);
}

TEST_CASE (PreflightThroughputTest_FormatsRemainingTimeAsHoursMinutes)
{
    // 1 GB/s required, 3.5 hours of free space worth of bytes.
    double bytesPerSec = 1024.0 * 1024.0 * 1024.0;
    uint64_t freeBytes = static_cast<uint64_t> (bytesPerSec * 3600.0 * 3.5);
    std::string formatted = PreflightThroughputTest::formatRemainingTime (freeBytes, bytesPerSec);
    REQUIRE (formatted == "3h 30m");
}

TEST_CASE (PreflightThroughputTest_CacheExpiresAfter30Days)
{
    REQUIRE_FALSE (PreflightThroughputTest::isCacheExpired (29.9));
    REQUIRE (PreflightThroughputTest::isCacheExpired (30.0));
    REQUIRE (PreflightThroughputTest::isCacheExpired (31.0));
}

TEST_CASE (PreflightThroughputTest_FlagsFat32ForReformat)
{
    REQUIRE (PreflightThroughputTest::needsReformat (PreflightThroughputTest::FilesystemKind::FAT32));
    REQUIRE_FALSE (PreflightThroughputTest::needsReformat (PreflightThroughputTest::FilesystemKind::ExFAT));
}

TEST_CASE (PreflightThroughputTest_VideoAddsToTheRequiredRate)
{
    // Eight microphones, 24-bit, 48k: stems plus the mix file.
    const double audioOnly = PreflightThroughputTest::requiredBytesPerSecond (8, 48000.0, 3);
    REQUIRE (audioOnly == 8.0 * 48000.0 * 3.0 * 2.0);

    // A camera writes once, so it is added rather than doubled -- the x2 above
    // is the stems plus the mix, and there is only one copy of the video.
    const double withVideo = PreflightThroughputTest::requiredBytesPerSecond (8, 48000.0, 3, 4'000'000.0);
    REQUIRE (withVideo == audioOnly + 4'000'000.0);

    // The default keeps every existing caller on the audio-only figure.
    REQUIRE (PreflightThroughputTest::requiredBytesPerSecond (8, 48000.0, 3, 0.0) == audioOnly);
}

TEST_CASE (PreflightThroughputTest_ACardThatPassesForAudioCanFailOnceACameraIsOn)
{
    // This is the whole point of the change. The card is genuinely fast enough
    // for eight microphones and genuinely not fast enough for eight microphones
    // plus a camera, and §6.4 says that has to be refused before the take rather
    // than discovered during it.
    const double audioGate = PreflightThroughputTest::requiredBytesPerSecond (8, 48000.0, 3)
                           * PreflightThroughputTest::kRequiredMultiplier;
    const double measured = audioGate * 1.1;

    const auto audioOnly = PreflightThroughputTest::evaluateMeasured (measured, 8, 48000.0, 3);
    REQUIRE (audioOnly.passed);

    const auto withCamera = PreflightThroughputTest::evaluateMeasured (measured, 8, 48000.0, 3, 4'000'000.0);
    REQUIRE_FALSE (withCamera.passed);

    // §10.6: the way out is named, because a number alone does not tell someone
    // that the camera is what put them over.
    REQUIRE (withCamera.reason.find ("cameras off") != std::string::npos);
}

TEST_CASE (PreflightThroughputTest_ACardTooSlowForTheAudioAloneIsNotBlamedOnTheCamera)
{
    // Turning the cameras off would not save this one, so it must not be offered
    // as a fix -- §10.6 forbids advice that does not work.
    const auto result = PreflightThroughputTest::evaluateMeasured (1024.0, 8, 48000.0, 3, 4'000'000.0);

    REQUIRE_FALSE (result.passed);
    REQUIRE (result.reason.find ("cameras off") == std::string::npos);
}

TEST_CASE (PreflightThroughputTest_EvaluateAgreesWithTheMeasuredForm)
{
    // evaluate() is now the windowed front end of evaluateMeasured(); the two
    // must not drift apart.
    const std::vector<double> windows { 30e6, 24e6, 41e6 };

    const auto windowed = PreflightThroughputTest::evaluate (windows, 4, 48000.0, 3, 4'000'000.0);
    const auto measured = PreflightThroughputTest::evaluateMeasured (24e6, 4, 48000.0, 3, 4'000'000.0);

    REQUIRE (windowed.passed == measured.passed);
    REQUIRE (windowed.requiredBytesPerSec == measured.requiredBytesPerSec);
    REQUIRE (windowed.sustainedMinBytesPerSec == measured.sustainedMinBytesPerSec);
}

TEST_CASE (PreflightThroughputTest_CachedWriteFailureIsRefusedAsCouldNotWriteNotTooSlow)
{
    // No windows measured: the old gate read 0 MB/s and called the card slow.
    PreflightResult empty;
    empty.couldNotWrite = true;
    empty.reason = "Couldn't write to this card.";

    const auto verdict = PreflightThroughputTest::evaluateCached (empty, 2, 48000.0, 3);
    REQUIRE_FALSE (verdict.passed);
    REQUIRE (verdict.couldNotWrite);
    REQUIRE (verdict.reason.find ("Couldn't write") != std::string::npos);
    REQUIRE (verdict.reason.find ("slow") == std::string::npos);
}

TEST_CASE (PreflightThroughputTest_CachedWriteFailureCannotPassOnAPartialMeasurement)
{
    // A fast card that took a few seconds of writes and then refused the rest.
    PreflightResult partial;
    partial.couldNotWrite = true;
    partial.sustainedMinBytesPerSec = 1.0e9;
    partial.reason = "Couldn't write to this card.";

    REQUIRE_FALSE (PreflightThroughputTest::evaluateCached (partial, 2, 48000.0, 3).passed);
}

TEST_CASE (PreflightThroughputTest_CachedMeasurementIsReappliedToTheCurrentRig)
{
    PreflightResult measured;
    measured.sustainedMinBytesPerSec = PreflightThroughputTest::requiredBytesPerSecond (2, 48000.0, 3) * 3.0;

    REQUIRE (PreflightThroughputTest::evaluateCached (measured, 2, 48000.0, 3).passed);
    REQUIRE_FALSE (PreflightThroughputTest::evaluateCached (measured, 8, 48000.0, 3).passed);
}

TEST_CASE (PreflightThroughputTest_WriteFailureIsRetriedAfterTheIntervalWhileIdle)
{
    PreflightResult failed;
    failed.couldNotWrite = true;

    const auto interval = PreflightThroughputTest::kWriteFailureRetrySeconds;

    // Not on every UI tick.
    REQUIRE_FALSE (PreflightThroughputTest::shouldRetryCached (failed, interval * 0.5, true, false));
    // After the interval: the user may have freed space or reinserted the card.
    REQUIRE (PreflightThroughputTest::shouldRetryCached (failed, interval, true, false));
    // Never mid-take, and never over a worker that is still running.
    REQUIRE_FALSE (PreflightThroughputTest::shouldRetryCached (failed, interval * 10.0, false, false));
    REQUIRE_FALSE (PreflightThroughputTest::shouldRetryCached (failed, interval * 10.0, true, true));
}

TEST_CASE (PreflightThroughputTest_SpeedVerdictsAreNotRetried)
{
    PreflightResult slow;
    slow.passed = false;
    slow.reason = "too slow";

    PreflightResult fast;
    fast.passed = true;

    REQUIRE_FALSE (PreflightThroughputTest::shouldRetryCached (slow, 3600.0, true, false));
    REQUIRE_FALSE (PreflightThroughputTest::shouldRetryCached (fast, 3600.0, true, false));
}

TEST_CASE (PreflightThroughputTest_NamesTheDriveFormatFromWhatTheOsCallsIt)
{
    using K = PreflightThroughputTest::FilesystemKind;
    REQUIRE (PreflightThroughputTest::filesystemKindFromTypeName ("msdos") == K::FAT32);
    REQUIRE (PreflightThroughputTest::filesystemKindFromTypeName ("vfat") == K::FAT32);
    REQUIRE (PreflightThroughputTest::filesystemKindFromTypeName ("exfat") == K::ExFAT);
    REQUIRE (PreflightThroughputTest::filesystemKindFromTypeName ("apfs") == K::APFS);
    REQUIRE (PreflightThroughputTest::filesystemKindFromTypeName ("hfs") == K::HFSPlus);
    REQUIRE (PreflightThroughputTest::filesystemKindFromTypeName ("") == K::Unknown);
    REQUIRE (PreflightThroughputTest::filesystemKindFromTypeName ("smbfs") == K::Other);
}

TEST_CASE (PreflightThroughputTest_AFat32CardRefusesOnlyWhenACameraWouldOutgrowIt)
{
    // The WAVs split at 3.9 GB; a camera movie is one file that dies at 4 GiB.
    REQUIRE_FALSE (PreflightThroughputTest::fileSizeRefusal (true, 1).empty());
    REQUIRE (PreflightThroughputTest::fileSizeRefusal (true, 0).empty());
    REQUIRE (PreflightThroughputTest::fileSizeRefusal (false, 2).empty());
}

TEST_CASE (PreflightThroughputTest_TheDriveFormatSurvivesReapplyingTheGate)
{
    PreflightResult cached;
    cached.passed = true;
    cached.sustainedMinBytesPerSec = 200.0e6;
    cached.limitedTo4GiBFiles = true;

    const auto verdict = PreflightThroughputTest::evaluateCached (cached, 2, 48000.0, 3, 4.0e6);
    REQUIRE (verdict.passed);
    REQUIRE (verdict.limitedTo4GiBFiles);
}

TEST_CASE (PreflightThroughputTest_APrivacyRefusalIsNotReportedAsACardFault)
{
    // macOS "Don't Allow" on files on a removable volume arrives as EPERM.
    const auto denied = PreflightThroughputTest::writeFailureReason (EPERM);
    REQUIRE (denied.find ("Privacy & Security") != std::string::npos);
    REQUIRE (denied.find ("plugged in") == std::string::npos);
    REQUIRE (PreflightThroughputTest::writeFailureReason (EACCES) == denied);

    REQUIRE (PreflightThroughputTest::writeFailureReason (EROFS).find ("read-only") != std::string::npos);
    REQUIRE (PreflightThroughputTest::writeFailureReason (ENOSPC).find ("full") != std::string::npos);
    REQUIRE (PreflightThroughputTest::writeFailureReason (0).find ("plugged in") != std::string::npos);
}

TEST_CASE (PreflightThroughputTest_AReadOnlyWindowsDriveIsNotCalledALockedCard)
{
    // macOS mounts NTFS read-only. "Slide its lock switch off" sent people
    // looking for a switch an external SSD does not have.
    using Kind = PreflightThroughputTest::FilesystemKind;
    const auto ntfs = PreflightThroughputTest::writeFailureReason (EROFS, Kind::NTFS);
    REQUIRE (ntfs.find ("NTFS") != std::string::npos);
    REQUIRE (ntfs.find ("lock switch") == std::string::npos);

    // A locked SD card is still a locked SD card.
    REQUIRE (PreflightThroughputTest::writeFailureReason (EROFS, Kind::ExFAT).find ("lock switch")
             != std::string::npos);
    REQUIRE (PreflightThroughputTest::writeFailureReason (EROFS, Kind::FAT32).find ("lock switch")
             != std::string::npos);

    // Other refusals on an NTFS drive keep their own wording.
    REQUIRE (PreflightThroughputTest::writeFailureReason (ENOSPC, Kind::NTFS).find ("full")
             != std::string::npos);
}

// §6.4's verdict is about the card it measured. A different card at the same
// path -- or no card -- makes it say nothing, but only on evidence gathered
// after the verdict: a look begun while the check was still running, or a
// status worker stuck on an old answer, must not keep throwing fresh verdicts
// away.
TEST_CASE (PreflightThroughputTest_AVerdictIsOnlyForTheCardItMeasured)
{
    using Clock = std::chrono::steady_clock;

    VolumeFingerprint exfatCard;
    exfatCard.known = true;
    exfatCard.mountRoot = "/Volumes/NO NAME";
    exfatCard.fsType = "exfat";
    exfatCard.device = "/dev/disk4s1";
    exfatCard.uuid = "6A1F-0C22";
    exfatCard.totalBytes = 64ull * 1000 * 1000 * 1000;

    auto fat32Card = exfatCard;
    fat32Card.fsType = "msdos";
    fat32Card.uuid = "51B0-9E14";
    fat32Card.totalBytes = 32ull * 1000 * 1000 * 1000;

    PreflightResult verdict;
    verdict.passed = true;
    verdict.measuredVolume = exfatCard;
    verdict.measuredVolumeConfirmedAt = Clock::now();

    const auto after = verdict.measuredVolumeConfirmedAt + std::chrono::milliseconds (500);
    const auto before = verdict.measuredVolumeConfirmedAt - std::chrono::milliseconds (500);

    REQUIRE_FALSE (PreflightThroughputTest::measuredAnotherDrive (verdict, exfatCard, after));
    REQUIRE (PreflightThroughputTest::measuredAnotherDrive (verdict, fat32Card, after));

    // Older than the verdict: not weighed against it.
    REQUIRE_FALSE (PreflightThroughputTest::measuredAnotherDrive (verdict, fat32Card, before));
    REQUIRE_FALSE (PreflightThroughputTest::measuredAnotherDrive (
        verdict, fat32Card, verdict.measuredVolumeConfirmedAt));

    // Nothing to compare: a verdict that measured nothing, or a look that
    // could not tell.
    PreflightResult nothingMeasured;
    nothingMeasured.couldNotWrite = true;
    REQUIRE_FALSE (PreflightThroughputTest::measuredAnotherDrive (nothingMeasured, fat32Card, after));
    REQUIRE_FALSE (PreflightThroughputTest::measuredAnotherDrive (verdict, VolumeFingerprint {}, after));
}

// The bug: a card that sustains 1.5 MB/s wrote all 200 MB of its test --
// over two minutes, Record waiting -- though its first second already
// settled the verdict, the slowest second. Mutation: never settled, or
// settled at the gate itself (a card exactly fast enough cut short).
TEST_CASE (PreflightThroughputTest_ASecondUnderTheGateSettlesTheTest)
{
    const double gate = PreflightThroughputTest::gateBytesPerSecond (4, 48000.0, 3, 8.0 * 1024 * 1024);
    REQUIRE_NEAR (gate, PreflightThroughputTest::requiredBytesPerSecond (4, 48000.0, 3, 8.0 * 1024 * 1024)
                            * PreflightThroughputTest::kRequiredMultiplier, 1e-6);

    REQUIRE (PreflightThroughputTest::settledBelow (1.5 * 1024 * 1024, gate));
    REQUIRE (PreflightThroughputTest::settledBelow (gate - 1.0, gate));
    REQUIRE_FALSE (PreflightThroughputTest::settledBelow (gate, gate));
    REQUIRE_FALSE (PreflightThroughputTest::settledBelow (gate * 3.0, gate));

    // Not known: the whole test, as before.
    REQUIRE_FALSE (PreflightThroughputTest::settledBelow (1.0, 0.0));
}

// A test stopped early answers the rig it was stopped for and any needing
// more; one needing no more than its slowest second -- a camera turned off --
// would pass on half a test, so the card is checked again. Mutation: a cut
// short result passed for a lighter rig, or re-measured for the same rig.
TEST_CASE (PreflightThroughputTest_ATestStoppedEarlyAnswersOnlyTheRigsItSettled)
{
    const double camera = 8.0 * 1024 * 1024;
    const double withCamera = PreflightThroughputTest::gateBytesPerSecond (4, 48000.0, 3, camera);
    const double audioOnly = PreflightThroughputTest::gateBytesPerSecond (4, 48000.0, 3);

    PreflightResult cut;
    cut.cutShort = true;
    cut.sustainedMinBytesPerSec = (audioOnly + withCamera) / 2.0;

    // The rig it was stopped for, and one needing more: settled, refused.
    REQUIRE_FALSE (PreflightThroughputTest::needsMeasuringFor (cut, 4, 48000.0, 3, camera));
    REQUIRE_FALSE (PreflightThroughputTest::needsMeasuringFor (cut, 8, 48000.0, 3, camera));
    const auto refused = PreflightThroughputTest::evaluateCached (cut, 4, 48000.0, 3, camera);
    REQUIRE_FALSE (refused.passed);
    REQUIRE (refused.reason.rfind ("This card is too slow", 0) == 0);

    // The camera off: its slowest second would pass. Not passed; checked again.
    REQUIRE (PreflightThroughputTest::needsMeasuringFor (cut, 4, 48000.0, 3));
    const auto unmeasured = PreflightThroughputTest::evaluateCached (cut, 4, 48000.0, 3);
    REQUIRE_FALSE (unmeasured.passed);
    REQUIRE (unmeasured.reason == "Checking this drive is fast enough...");

    // A whole test answers every rig; a card that couldn't be written is that.
    auto whole = cut;
    whole.cutShort = false;
    REQUIRE_FALSE (PreflightThroughputTest::needsMeasuringFor (whole, 4, 48000.0, 3));
    REQUIRE (PreflightThroughputTest::evaluateCached (whole, 4, 48000.0, 3).passed);
    auto unwritable = cut;
    unwritable.couldNotWrite = true;
    REQUIRE_FALSE (PreflightThroughputTest::needsMeasuringFor (unwritable, 4, 48000.0, 3));
}
