#include "TestFramework.h"
#include "Core/CameraSelection.h"

using namespace mma;

namespace {

std::vector<CameraDeviceInfo> twoCameras()
{
    return { { "cam-a", "Logitech C920" }, { "cam-b", "Built-in Camera" } };
}

} // namespace

TEST_CASE (CameraSelection_everyCameraPluggedInRecordsUnlessSwitchedOff)
{
    CameraSelection selection;
    selection.setAvailableCameras (twoCameras());

    // Plugging a camera in is enough: it is in the take, in its own file,
    // until someone switches it off in the Cameras panel.
    REQUIRE (selection.isEnabled ("cam-a"));
    REQUIRE (selection.isEnabled ("cam-b"));
    REQUIRE (selection.getEnabledCount() == 2);
    REQUIRE (selection.buildPlans().size() == 2);
}

TEST_CASE (CameraSelection_noCamerasEnablesNothing)
{
    CameraSelection selection;
    selection.setAvailableCameras ({});

    REQUIRE (selection.getEnabledCount() == 0);
    REQUIRE (selection.buildPlans().empty());
}

TEST_CASE (CameraSelection_aCameraSwitchedOffStaysOffWhenAnotherIsPluggedIn)
{
    CameraSelection selection;
    selection.setAvailableCameras ({ { "cam-a", "Logitech C920" } });
    selection.setEnabled ("cam-a", false);

    // A later discovery must not undo a choice: the switched-off camera stays
    // off, and only the newcomer joins.
    selection.setAvailableCameras (twoCameras());

    REQUIRE_FALSE (selection.isEnabled ("cam-a"));
    REQUIRE (selection.isEnabled ("cam-b"));
    REQUIRE (selection.getEnabledCount() == 1);
}

TEST_CASE (CameraSelection_choicesSurviveAnUnplug)
{
    CameraSelection selection;
    selection.setAvailableCameras (twoCameras());
    selection.setEnabled ("cam-a", true);
    selection.setEnabled ("cam-b", true);
    selection.setAssignedName ("cam-b", "Wide shot");

    // Unplugged...
    selection.setAvailableCameras ({ { "cam-a", "Logitech C920" } });
    REQUIRE (selection.getEnabledCount() == 1);

    // ...and back, with the answer the user already gave about it.
    selection.setAvailableCameras (twoCameras());
    REQUIRE (selection.isEnabled ("cam-b"));
    REQUIRE (selection.getDisplayName ("cam-b") == std::string ("Wide shot"));
}

TEST_CASE (CameraSelection_anEnabledUnpluggedCameraRemainsVisibleAsUnavailable)
{
    CameraSelection selection;
    selection.setAvailableCameras ({ { "USB2 Video", "USB2 Video" } });
    selection.setEnabled ("USB2 Video", true);

    selection.setAvailableCameras ({});

    const auto missing = selection.getUnavailableEnabledCameras();
    REQUIRE (missing.size() == 1);
    REQUIRE (missing.front().id == std::string ("USB2 Video"));
    REQUIRE (missing.front().displayName == std::string ("USB2 Video"));
    REQUIRE (selection.getEnabledCount() == 0);
    const auto plans = selection.buildPlans();
    REQUIRE (plans.empty());

    const auto intendedPlans = selection.buildIntendedPlans();
    REQUIRE (intendedPlans.size() == 1);
    REQUIRE (intendedPlans.front().deviceId == std::string ("USB2 Video"));
    REQUIRE (intendedPlans.front().fileName == std::string ("V01_USB2-Video"));
}

TEST_CASE (CameraSelection_aRememberedNameLabelsAnUnavailableCamera)
{
    CameraSelection selection;
    selection.setEnabled ("capture-card-id", true);
    selection.setAssignedName ("capture-card-id", "Stage wide");

    const auto missing = selection.getUnavailableEnabledCameras();
    REQUIRE (missing.size() == 1);
    REQUIRE (missing.front().displayName == std::string ("Stage wide"));
}

TEST_CASE (CameraSelection_disablingAnUnavailableCameraUpdatesRememberedState)
{
    CameraSelection selection;
    selection.setEnabled ("USB2 Video", true);
    selection.setAssignedName ("USB2 Video", "HDMI wide");
    REQUIRE (selection.getUnavailableEnabledCameras().size() == 1);

    selection.setEnabled ("USB2 Video", false);

    REQUIRE (selection.getUnavailableEnabledCameras().empty());
    const auto known = selection.getKnownCameras();
    REQUIRE (known.size() == 1);
    REQUIRE (known.front().id == std::string ("USB2 Video"));
    REQUIRE (known.front().displayName == std::string ("HDMI wide"));
    REQUIRE_FALSE (selection.isEnabled (known.front().id));
}

TEST_CASE (CameraSelection_planNamesFollowTheSessionNamingRules)
{
    CameraSelection selection;
    selection.setAvailableCameras (twoCameras());
    selection.setEnabled ("cam-a", true);
    selection.setEnabled ("cam-b", true);
    selection.setAssignedName ("cam-a", "Kitchen / Wide!!");

    const auto plans = selection.buildPlans();

    REQUIRE ((int) plans.size() == 2);
    // §6.2 sanitizing, and a V prefix so the pictures do not land in the middle
    // of the numbered audio stems when the folder is sorted by name.
    REQUIRE (plans[0].fileName == std::string ("V01_Kitchen-Wide"));
    REQUIRE (plans[1].fileName == std::string ("V02_Built-in-Camera"));
    REQUIRE (plans[0].deviceId == std::string ("cam-a"));
}

TEST_CASE (CameraSelection_disabledCamerasAreNotNumbered)
{
    CameraSelection selection;
    selection.setAvailableCameras (twoCameras());
    selection.setEnabled ("cam-a", false);
    selection.setEnabled ("cam-b", true);

    const auto plans = selection.buildPlans();

    // The one camera in the take is V01, not V02 -- the number counts what is
    // being recorded, not what happens to be plugged in.
    REQUIRE ((int) plans.size() == 1);
    REQUIRE (plans[0].fileName == std::string ("V01_Built-in-Camera"));
}

TEST_CASE (CameraSelection_videoCountsAgainstRemainingTime)
{
    CameraSelection selection;
    REQUIRE (selection.getEstimatedBytesPerSecond() == (int64_t) 0);

    // Two cameras plugged in are two cameras recording, and both count
    // against the room left on the card.
    selection.setAvailableCameras (twoCameras());
    REQUIRE (selection.getEstimatedBytesPerSecond() == 2 * CameraSelection::kEstimatedVideoBytesPerSecond);

    selection.setEnabled ("cam-b", false);
    REQUIRE (selection.getEstimatedBytesPerSecond() == CameraSelection::kEstimatedVideoBytesPerSecond);
}

TEST_CASE (CameraSelection_qualityCapsSizeAndBudget)
{
    CameraSelection selection;
    selection.setAvailableCameras (twoCameras());

    REQUIRE (selection.getQuality ("cam-a") == CameraQuality::Best);
    REQUIRE (CameraSelection::frameLimitFor (CameraQuality::Best).maxHeight == 2160);
    REQUIRE (CameraSelection::frameLimitFor (CameraQuality::HD1080).maxHeight == 1080);
    REQUIRE (CameraSelection::frameLimitFor (CameraQuality::HD720).maxHeight == 720);

    // A camera turned down to 720p costs the card less than one left at Best.
    selection.setQuality ("cam-b", CameraQuality::HD720);
    REQUIRE (selection.getEstimatedBytesPerSecond()
             == CameraSelection::kEstimatedVideoBytesPerSecond
                + CameraSelection::estimatedBytesPerSecondFor (CameraQuality::HD720));
    REQUIRE (CameraSelection::estimatedBytesPerSecondFor (CameraQuality::HD720)
             < CameraSelection::estimatedBytesPerSecondFor (CameraQuality::HD1080));

    // Unplugging forgets nothing.
    selection.setAvailableCameras ({});
    selection.setAvailableCameras (twoCameras());
    REQUIRE (selection.getQuality ("cam-b") == CameraQuality::HD720);
}

TEST_CASE (CameraSelection_qualityKeysRoundTrip)
{
    for (const auto q : { CameraQuality::Best, CameraQuality::HD1080, CameraQuality::HD720 })
        REQUIRE (cameraQualityFromKey (cameraQualityKey (q)) == q);

    REQUIRE (cameraQualityFromKey ("8k-someday") == CameraQuality::Best);
}

TEST_CASE (CameraSelection_previewSizeNeverDrivesCaptureSize)
{
    // The only thing the preview setting decides is how tall the picture is
    // drawn. Both modes exist so the live view can be cheap; neither is
    // consulted about what gets written.
    REQUIRE (CameraSelection::previewSettingsFor (PreviewQuality::Low).maxViewHeight
                     < CameraSelection::previewSettingsFor (PreviewQuality::Full).maxViewHeight);
    REQUIRE (CameraSelection::previewSettingsFor (PreviewQuality::Low).maxViewHeight > 0);
}

// ---------------------------------------------------------------------------
// Per-device ids (macOS uniqueID) and the choices remembered before them,
// under "Name" and "Name #2" by OS order.

namespace {

/// Seeds a choice the way the app does from settings.json at launch.
void remember (CameraSelection& selection, const std::string& id, bool enabled,
               const std::string& name = {}, CameraQuality quality = CameraQuality::Best)
{
    selection.setEnabled (id, enabled);
    selection.setQuality (id, quality);
    if (! name.empty())
        selection.setAssignedName (id, name);
}

} // namespace

TEST_CASE (CameraSelection_aSingleOldChoiceForTheNameIsTakenOverByItsCamera)
{
    CameraSelection selection;
    remember (selection, "Logitech C920", false, "Wide", CameraQuality::HD720);

    selection.setAvailableCameras ({ { "0x1420000046d0825", "Logitech C920", "Logitech C920" } });

    REQUIRE_FALSE (selection.isEnabled ("0x1420000046d0825"));
    REQUIRE (selection.getDisplayName ("0x1420000046d0825") == "Wide");
    REQUIRE (selection.getQuality ("0x1420000046d0825") == CameraQuality::HD720);
    REQUIRE (selection.takeAdoptedLegacyChoices());
    REQUIRE_FALSE (selection.takeAdoptedLegacyChoices());

    // The old entry is gone, not left behind as a phantom missing camera.
    REQUIRE (selection.getKnownCameras().size() == 1);
    REQUIRE (selection.resolveId ("Logitech C920") == "0x1420000046d0825");
}

TEST_CASE (CameraSelection_aSingleOldChoiceIsTakenEvenIfItWasTheSecondTwin)
{
    // Only "#2" was remembered. Exactly one match: it belongs to the camera
    // of that name now connected.
    CameraSelection selection;
    remember (selection, "Twin Cam #2", false, "Left");

    selection.setAvailableCameras ({ { "id-a", "Twin Cam", "Twin Cam" } });

    REQUIRE_FALSE (selection.isEnabled ("id-a"));
    REQUIRE (selection.getDisplayName ("id-a") == "Left");
}

TEST_CASE (CameraSelection_ambiguousOldChoicesFallBackToOsOrderOnce)
{
    // Two remembered twins: the old ids cannot say which unit was which, so
    // the OS order decides this once, exactly as it always did...
    CameraSelection selection;
    remember (selection, "Twin Cam", true, "Left");
    remember (selection, "Twin Cam #2", false, "Right");

    selection.setAvailableCameras ({ { "id-b", "Twin Cam", "Twin Cam" },
                                     { "id-a", "Twin Cam", "Twin Cam #2" } });

    REQUIRE (selection.getDisplayName ("id-b") == "Left");
    REQUIRE (selection.isEnabled ("id-b"));
    REQUIRE (selection.getDisplayName ("id-a") == "Right");
    REQUIRE_FALSE (selection.isEnabled ("id-a"));

    // ...and from then on the choices follow the device, whatever the order.
    selection.setAvailableCameras ({ { "id-a", "Twin Cam", "Twin Cam" },
                                     { "id-b", "Twin Cam", "Twin Cam #2" } });
    REQUIRE (selection.getDisplayName ("id-b") == "Left");
    REQUIRE (selection.getDisplayName ("id-a") == "Right");
    REQUIRE_FALSE (selection.isEnabled ("id-a"));
}

TEST_CASE (CameraSelection_ambiguousOldChoicesWithOneTwinPluggedInKeepTheOldMapping)
{
    CameraSelection selection;
    remember (selection, "Twin Cam", true, "Left");
    remember (selection, "Twin Cam #2", false, "Right");

    // One twin, first in the list: it gets what "Twin Cam" always gave it,
    // and "#2" waits for the other unit.
    selection.setAvailableCameras ({ { "id-a", "Twin Cam", "Twin Cam" } });
    REQUIRE (selection.getDisplayName ("id-a") == "Left");

    selection.setAvailableCameras ({ { "id-a", "Twin Cam", "Twin Cam" },
                                     { "id-b", "Twin Cam", "Twin Cam #2" } });
    REQUIRE (selection.getDisplayName ("id-b") == "Right");
    REQUIRE_FALSE (selection.isEnabled ("id-b"));
}

TEST_CASE (CameraSelection_unpluggingOneTwinHandsNothingToTheOther)
{
    CameraSelection selection;
    selection.setAvailableCameras ({ { "id-a", "Twin Cam", "Twin Cam" },
                                     { "id-b", "Twin Cam", "Twin Cam #2" } });
    selection.setEnabled ("id-a", false);
    selection.setAssignedName ("id-a", "Left");
    selection.setQuality ("id-a", CameraQuality::HD720);
    selection.setAssignedName ("id-b", "Right");

    // id-a unplugged: id-b is now first in the list, the old "Twin Cam".
    selection.setAvailableCameras ({ { "id-b", "Twin Cam", "Twin Cam" } });

    REQUIRE (selection.isEnabled ("id-b"));
    REQUIRE (selection.getDisplayName ("id-b") == "Right");
    REQUIRE (selection.getQuality ("id-b") == CameraQuality::Best);
    REQUIRE_FALSE (selection.takeAdoptedLegacyChoices());
}

TEST_CASE (CameraSelection_aCameraWithoutItsOwnIdKeepsItsNameBasedChoices)
{
    // A camera that still goes by its name keeps its entry; a twin with a
    // per-device id cannot take it from under it.
    CameraSelection selection;
    remember (selection, "Twin Cam", false, "Old");

    selection.setAvailableCameras ({ { "Twin Cam", "Twin Cam" },
                                     { "id-b", "Twin Cam", "Twin Cam #2" } });

    REQUIRE_FALSE (selection.isEnabled ("Twin Cam"));
    REQUIRE (selection.getDisplayName ("Twin Cam") == "Old");
    REQUIRE (selection.isEnabled ("id-b"));
    REQUIRE (selection.getDisplayName ("id-b") == "Twin Cam");
}

TEST_CASE (CameraSelection_otherNamesAreNeverTakenOver)
{
    CameraSelection selection;
    remember (selection, "Twin Cam Pro", false);
    remember (selection, "Twin Cam #02", false);
    remember (selection, "Twin Cam #1", false);

    selection.setAvailableCameras ({ { "id-a", "Twin Cam", "Twin Cam" } });

    REQUIRE (selection.isEnabled ("id-a")); // fresh: seen for the first time
    REQUIRE_FALSE (selection.takeAdoptedLegacyChoices());
    REQUIRE (selection.resolveId ("Twin Cam Pro") == "Twin Cam Pro");
}

TEST_CASE (CameraSelection_SpaceAnHour_FollowsTheBudget)
{
    // The Cameras page's figure comes from the same per-camera budget the
    // free-space and card checks use: decimal GB, rounded to whole ones.
    REQUIRE (CameraSelection::spaceAnHourText (
                 static_cast<double> (CameraSelection::estimatedBytesPerSecondFor (CameraQuality::Best)))
             == "Recording now takes up to about 29 GB an hour, sound included.");
    REQUIRE (CameraSelection::spaceAnHourText (
                 static_cast<double> (CameraSelection::estimatedBytesPerSecondFor (CameraQuality::HD1080)))
             == "Recording now takes up to about 14 GB an hour, sound included.");
    REQUIRE (CameraSelection::spaceAnHourText (
                 static_cast<double> (CameraSelection::estimatedBytesPerSecondFor (CameraQuality::HD720)))
             == "Recording now takes up to about 7 GB an hour, sound included.");

    // A bump to the estimate moves the text: the figure is worked out, not
    // written in.
    REQUIRE (CameraSelection::spaceAnHourText (
                 static_cast<double> (CameraSelection::estimatedBytesPerSecondFor (CameraQuality::Best)) + 1.0e6)
             == "Recording now takes up to about 32 GB an hour, sound included.");

    // One 24-bit, 48 kHz track is half a gigabyte an hour, and says so
    // rather than "about 1 GB" or "about 0 GB".
    REQUIRE (CameraSelection::spaceAnHourText (48000.0 * 3.0)
             == "Recording now takes up to less than 1 GB an hour, sound included.");
    REQUIRE (CameraSelection::spaceAnHourText (0.0)
             == "Recording now takes up to less than 1 GB an hour, sound included.");
}
