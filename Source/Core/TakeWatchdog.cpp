#include "TakeWatchdog.h"

namespace mma {

void TakeWatchdog::beginTake (const TakeHealth& baseline)
{
    watching = true;
    last = baseline;
    warnedTenMinutes = false;
    warnedTwoMinutes = false;
    droppedReported = false;
    droppedReportedAt = 0.0;
    lostCamerasThisTake.clear();
}

void TakeWatchdog::endTake()
{
    watching = false;
    last = {};
    lostCamerasThisTake.clear();
}

std::vector<TakeAlert> TakeWatchdog::observe (const TakeHealth& now)
{
    std::vector<TakeAlert> alerts;

    if (! watching)
        return alerts;

    // Microphones, matched by position: the channel list is fixed for the
    // whole take (§6.5), so index i is the same person throughout.
    for (size_t i = 0; i < now.mics.size(); ++i)
    {
        const bool wasLive = i < last.mics.size() ? last.mics[i].live : true;
        const auto& mic = now.mics[i];

        if (wasLive && ! mic.live)
            alerts.push_back ({ TakeAlert::Kind::MicLost,
                                mic.name + " has stopped sending sound -- unplugged, or its cable "
                                "or mixer channel has gone. The take carries on; its file gets "
                                "silence until it comes back.", false });
        else if (! wasLive && mic.live)
            alerts.push_back ({ TakeAlert::Kind::MicBack, mic.name + " is back and recording again.", true });
    }

    // Cameras, matched by device id: a camera that is no longer enumerated
    // has been unplugged or switched off, and its file stops growing. One
    // that has dropped out of the list altogether is carried forward as
    // absent. CameraController cannot append a reconnect to the interrupted
    // movie, so a loss is latched for this take even if a later OS snapshot
    // lists the same camera again.
    //
    // Not by name: two cameras of the same model share one, and the first
    // same-named entry answered for both. A twin that never started (no
    // HDMI signal) took its "was present" from the one recording, which
    // raised "has gone away" the moment Record was pressed, and the latch
    // then swallowed the real loss of the other later in the take.
    const auto keyOf = [] (const TakeHealth::Camera& c) -> const std::string&
    {
        return c.id.empty() ? c.name : c.id;
    };

    auto cameras = now.cameras;

    for (const auto& old : last.cameras)
    {
        bool listed = false;
        for (const auto& cam : cameras)
            if (keyOf (cam) == keyOf (old)) { listed = true; break; }

        if (! listed)
            cameras.push_back ({ old.name, false, old.id });
    }

    for (const auto& cam : cameras)
    {
        bool wasPresent = true;
        for (const auto& old : last.cameras)
            if (keyOf (old) == keyOf (cam)) { wasPresent = old.present; break; }

        if (lostCamerasThisTake.count (keyOf (cam)) > 0)
            continue;

        // There is no "and it came back" branch, and there cannot be one. The
        // latch above skips any camera already recorded as lost, so a genuine
        // return could never reach it; the only transition that could was
        // STARTING -> recording, which is a camera that never left. It said
        // "Camera X is back." at the top of a take, about nothing.
        //
        // A camera really cannot rejoin an interrupted movie -- see the note
        // above on why the loss is latched for the whole take -- so the honest
        // thing is that the message does not exist.
        if (wasPresent && ! cam.present)
        {
            lostCamerasThisTake.insert (keyOf (cam));
            alerts.push_back ({ TakeAlert::Kind::CameraLost,
                                "Camera " + cam.name + " has gone away -- unplugged or switched off. "
                                "The sound carries on; this camera stays out for the rest of this take "
                                "and can be used again on the next take.", false });
        }
    }

    for (auto& cam : cameras)
        if (lostCamerasThisTake.count (keyOf (cam)) > 0)
            cam.present = false;

    if (! now.cameraProblem.empty() && now.cameraProblem != last.cameraProblem)
        alerts.push_back ({ TakeAlert::Kind::CameraTrouble, now.cameraProblem, false });

    // An output set aside for changing its rate is still plugged in and was
    // not taken: the monitor problem (MonitorTrouble below) says what did
    // happen, and that it stays off until Stop.
    if (now.outputClockLost && ! last.outputClockLost && ! now.outputRateChanged)
        alerts.push_back ({ TakeAlert::Kind::OutputLost,
                            "The headphone output has stopped -- unplugged, or taken by another "
                            "app. The take carries on on this computer's clock; you will not hear "
                            "the mix until it comes back.", false });
    else if (! now.outputClockLost && last.outputClockLost)
        alerts.push_back ({ TakeAlert::Kind::OutputBack, "The headphone output is back.", true });

    if (now.framesDropped > last.framesDropped || now.samplesOverrun > last.samplesOverrun)
    {
        const bool due = ! droppedReported
                      || now.elapsedSeconds - droppedReportedAt >= kDroppedRepeatSeconds;

        if (due)
        {
            // Two losses of the same kind -- audio that should have been
            // recorded and was not -- so they add. The rate is the take's own;
            // a hardcoded 48000 made this line claim twice the loss at 96 kHz.
            const auto rate = now.sampleRate > 0.0 ? now.sampleRate : 48000.0;
            const auto total = now.framesDropped + now.samplesOverrun;
            const auto seconds = total / rate; // a rough figure is all this line needs

            alerts.push_back ({ TakeAlert::Kind::AudioDropped,
                                droppedReported
                                    ? "Sound is still being dropped: about " + std::to_string (static_cast<int> (seconds + 0.5))
                                          + " s lost so far. Close other apps."
                                    : "Some sound was dropped: this computer could not keep up for a moment. "
                                      "Close other apps. The take carries on.",
                                false });
            droppedReported = true;
            droppedReportedAt = now.elapsedSeconds;
        }
    }

    // Once only the mix is being written there are no separate tracks left to
    // drop; MixOnly has already said so, and this warning would promise a loss
    // that has happened.
    if (now.writerBehind && ! last.writerBehind && ! now.mixOnly)
        alerts.push_back ({ TakeAlert::Kind::WriterBehind,
                            "The drive is falling behind the microphones. If this keeps up the "
                            "separate tracks will be dropped and only the mix kept. A faster drive "
                            "or fewer other apps fixes it.", false });

    if (now.mixOnly && ! last.mixOnly)
        alerts.push_back ({ TakeAlert::Kind::MixOnly,
                            "The drive could not keep up. From here only the mix is being written, "
                            "not the separate tracks.", false });

    if (! now.monitorProblem.empty() && now.monitorProblem != last.monitorProblem)
        alerts.push_back ({ TakeAlert::Kind::MonitorTrouble, now.monitorProblem, false });

    // Room on the drive: each threshold once per take, on the way down.
    //
    // Strictly above zero. Zero used to be unreachable -- the app collapsed a
    // full drive into the "unknown" sentinel, which this gate excluded -- and
    // now that it is a real value it would fall into the two-minute arm and
    // tell someone with NO room left that they have about two minutes and
    // should wrap up. There is no room-exhausted alert kind here because the
    // take is stopped in that case, and the stop announces itself; what this
    // block must not do is contradict it a tick earlier.
    if (now.remainingSeconds > 0.0)
    {
        if (! warnedTwoMinutes && now.remainingSeconds < kTwoMinutes)
        {
            warnedTwoMinutes = true;
            warnedTenMinutes = true;
            alerts.push_back ({ TakeAlert::Kind::TwoMinutesLeft,
                                "About two minutes of room left on the drive. Wrap up now.", false });
        }
        else if (! warnedTenMinutes && now.remainingSeconds < kTenMinutes)
        {
            warnedTenMinutes = true;
            alerts.push_back ({ TakeAlert::Kind::TenMinutesLeft,
                                "About ten minutes of room left on the drive.", false });
        }
    }

    last = now;
    last.cameras = cameras;
    return alerts;
}

} // namespace mma
