#include "SetupGuide.h"
#include <algorithm>
#include <cmath>

namespace mma {

namespace {

std::string plural (int count, const char* one, const char* many)
{
    return std::to_string (count) + " " + (count == 1 ? one : many);
}

} // namespace

std::vector<SetupGuideStep> SetupGuide::stepsFor (const SetupGuideRig& rig)
{
    std::vector<SetupGuideStep> steps { SetupGuideStep::Microphones,
                                        SetupGuideStep::Names,
                                        SetupGuideStep::Headphones,
                                        SetupGuideStep::SaveFolder };

    // A page about cameras for a rig with none is a page about something the
    // reader does not have. One plugged in later records anyway, and the last
    // page says where to look.
    if (rig.cameraCount > 0)
        steps.push_back (SetupGuideStep::Cameras);

    steps.push_back (SetupGuideStep::TestTake);
    return steps;
}

SetupGuideStep SetupGuide::currentStep (const SetupGuideRig& rig) const
{
    // The enum is in page order, so "the next page that still exists" is the
    // first listed step at or after the one being shown. TestTake is always
    // listed, so there is always one.
    for (const auto step : stepsFor (rig))
        if (static_cast<int> (step) >= static_cast<int> (current))
            return step;

    return SetupGuideStep::TestTake;
}

bool SetupGuide::next (const SetupGuideRig& rig)
{
    const auto steps = stepsFor (rig);
    const auto here = std::find (steps.begin(), steps.end(), currentStep (rig));

    if (here == steps.end() || here + 1 == steps.end())
        return false;

    current = *(here + 1);
    return true;
}

void SetupGuide::back (const SetupGuideRig& rig)
{
    const auto steps = stepsFor (rig);
    const auto here = std::find (steps.begin(), steps.end(), currentStep (rig));

    if (here != steps.end() && here != steps.begin())
        current = *(here - 1);
}

SetupGuidePage SetupGuide::page (const SetupGuideRig& rig) const
{
    return pageFor (currentStep (rig), rig);
}

SetupGuidePage SetupGuide::pageFor (SetupGuideStep step, const SetupGuideRig& rig)
{
    const auto steps = stepsFor (rig);
    const auto at = std::find (steps.begin(), steps.end(), step);

    SetupGuidePage p;
    p.step = step;
    p.total = static_cast<int> (steps.size());
    p.number = at == steps.end() ? p.total : static_cast<int> (at - steps.begin()) + 1;
    p.canGoBack = p.number > 1;
    p.isLast = p.number == p.total;
    p.nextLabel = p.isLast ? "Done" : "Next";

    switch (step)
    {
        case SetupGuideStep::Microphones:
            p.title = "Plug in your microphones";
            p.body = "Plug each microphone into this computer with its USB cable. Every one "
                     "gets its own strip on the main screen, with a face that moves when it "
                     "hears sound. The computer's own built-in microphone is left out on "
                     "purpose.";

            if (rig.micCount == 0)
            {
                p.status = "No microphones yet. Plug one in and give it a few seconds to appear here.";
                p.statusIsWarning = true;
            }
            else
            {
                p.status = "Found " + plural (rig.micCount, "microphone", "microphones") + ".";
            }

            // A microphone that is plugged in but switched off looks, to
            // someone new, exactly like one the app cannot see.
            if (rig.switchedOffMicCount > 0)
            {
                p.status += " " + plural (rig.switchedOffMicCount, "more is", "more are")
                          + " switched off: open Settings and tick "
                          + (rig.switchedOffMicCount == 1 ? "its box" : "their boxes")
                          + " to use " + (rig.switchedOffMicCount == 1 ? "it." : "them.");
                p.statusIsWarning = true;
            }
            break;

        case SetupGuideStep::Names:
            p.title = "Name each microphone";

            if (rig.micCount <= 1)
                p.body = "Click Name below, or the name on the microphone's strip, and type "
                         "who will use it. The name goes on the recording, so you can tell "
                         "the files apart later.";
            else
                p.body = "Microphones of the same kind all look alike to the computer, so "
                         "find out which is which: tap the top of one with a finger. Its "
                         "strip on the main screen lights up, and so does its row below. "
                         "Click Name on that row, or the name on the strip, and type who "
                         "will use it. The name goes on that person's recording.";

            if (rig.micCount == 0)
            {
                p.status = "Plug in a microphone first. There is nothing to name yet.";
                p.statusIsWarning = true;
            }
            break;

        case SetupGuideStep::Headphones:
            p.title = "Headphones";

            if (rig.mixGoesToEveryMic)
            {
                p.body = "Plug a pair of headphones into the headphone socket on each "
                         "microphone. Everyone hears everyone, themselves included, in their "
                         "own pair. Turn the volume slider at the bottom of the main screen "
                         "down before anyone puts them on, then bring it up while someone "
                         "talks. Untick anyone below who would rather not hear the mix.";

                if (rig.headphoneJackCount == 0)
                {
                    p.status = rig.micCount == 0
                        ? "Once a microphone with a headphone socket is plugged in, it is listed here."
                        : "None of these microphones has a headphone socket the app can use. "
                          "Plug headphones into this computer instead, and pick them in "
                          "Settings under Output device.";
                    p.statusIsWarning = rig.micCount > 0;
                }
            }
            else
            {
                p.body = "The mix is going to the headphones picked in Settings, not to the "
                         "microphones' own sockets. To have everyone listen through their own "
                         "microphone instead, open Settings and pick the first entry under "
                         "Output device. Either way, turn the volume slider at the bottom of "
                         "the main screen down before anyone puts headphones on.";
            }
            break;

        case SetupGuideStep::SaveFolder:
            p.title = "Where recordings go";
            p.body = "Every take gets a folder of its own inside the folder below, named "
                     "with the date and time. Inside are one file per person and one with "
                     "everyone together. Pick somewhere you will find again; a memory card "
                     "or an external drive is fine.";
            break;

        case SetupGuideStep::Cameras:
            p.title = "Cameras";
            p.body = "Cameras plugged into this computer record with every take, each into "
                     "its own video file next to the sound, and their pictures show on the "
                     "main screen. To leave one out, or to give it a name, click Cameras at "
                     "the top of the window.";

            p.status = "Found " + plural (rig.cameraCount, "camera", "cameras");

            if (rig.camerasOn == rig.cameraCount)
                p.status += rig.cameraCount == 1 ? ". It will record." : ". All of them will record.";
            else if (rig.camerasOn == 0)
                p.status += rig.cameraCount == 1 ? ". It is switched off." : ". All are switched off.";
            else
                p.status += ". " + std::to_string (rig.camerasOn) + " will record.";
            break;

        case SetupGuideStep::TestTake:
            p.title = "Make a test take";
            p.body = "Press Start recording on the main screen, have everyone say a few "
                     "words for about ten seconds, then press it again to stop. A card then "
                     "shows the files it made and can open their folder, so you can listen "
                     "back. If that all works, you are ready.";

            if (rig.recording)
            {
                const int seconds = static_cast<int> (std::floor (std::max (0.0, rig.recordingSeconds)));

                p.status = seconds >= 10
                    ? "Recording: " + std::to_string (seconds) + " seconds. That's enough; press the button again to stop."
                    : "Recording: " + std::to_string (seconds) + (seconds == 1 ? " second." : " seconds.");
            }
            else if (rig.testTakeSaved)
            {
                p.status = "Your test take is saved. You are all set.";
            }
            else if (! rig.recordBlockedReason.empty())
            {
                p.status = "Recording can't start yet: " + rig.recordBlockedReason;
                p.statusIsWarning = true;
            }
            break;
    }

    return p;
}

} // namespace mma
