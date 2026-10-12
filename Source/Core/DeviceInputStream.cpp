#include "DeviceInputStream.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace mma {

static_assert (std::atomic<double>::is_always_lock_free,
               "DeviceInputStream requires lock-free double atomics on the audio thread");
static_assert (std::atomic<int64_t>::is_always_lock_free,
               "DeviceInputStream requires lock-free 64-bit atomics on the audio thread");

namespace {

int64_t steadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds> (
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<DeviceInputStream::NowNsFn> clockOverride { nullptr };
std::atomic<bool> virtualFillEnabled { true };

int64_t nowNs()
{
    const auto fn = clockOverride.load (std::memory_order_relaxed);
    return fn != nullptr ? fn() : steadyNowNs();
}

} // namespace

void DeviceInputStream::setClockForTesting (NowNsFn fn) noexcept
{
    clockOverride.store (fn, std::memory_order_relaxed);
}

void DeviceInputStream::setVirtualFillForTesting (bool enabled) noexcept
{
    virtualFillEnabled.store (enabled, std::memory_order_relaxed);
}

DeviceInputStream::DeviceInputStream (double sampleRate) noexcept
    : ring (static_cast<size_t> (kRingBlocks) * 64), compensator (sampleRate), rate (sampleRate)
{
}

size_t DeviceInputStream::usableCapacity() const noexcept
{
    const size_t block = std::max ({ nominalBlockSamples,
                                     largestPushSamples.load (std::memory_order_relaxed),
                                     largestPullSamples.load (std::memory_order_relaxed) });

    return std::min (ring.capacity(), block * static_cast<size_t> (kRingBlocks));
}

size_t DeviceInputStream::heldLatency() const noexcept
{
    // Before the first delivery the device's block is taken to be the one
    // asked for; it is replaced by the real one the moment one lands. A
    // provisional block counts too: the stream has already moved for it.
    const size_t block = seenDeviceBlock > 0 ? seenDeviceBlock : nominalBlockSamples;
    return std::max (block, provisionalBlock);
}

void DeviceInputStream::publishPlaceInTime() noexcept
{
    settledBlockPublished.store (seenDeviceBlock > 0 ? seenDeviceBlock : nominalBlockSamples,
                                 std::memory_order_relaxed);
    heldBlockPublished.store (heldLatency(), std::memory_order_relaxed);
}

void DeviceInputStream::recomputeTarget() noexcept
{
    baseTargetSamples = cushionSamples (std::max (nominalBlockSamples,
                                                  largestPullSamples.load (std::memory_order_relaxed)),
                                        rate);

    // The device's current block, which the de-quantized level already
    // discounts (virtualFillNow): the ring is held at the base target just
    // before each delivery, so a sample waits the target plus a block. What
    // this stream holds beyond its current block is headroom: the room a
    // larger block it has run at before needs, so a change of IO size moves
    // nothing in time and loses nothing.
    const auto lastPush = static_cast<size_t> (std::max (0, lastPushSamples.load (std::memory_order_acquire)));
    const size_t held = heldLatency();
    const size_t current = lastPush > 0 ? std::min (lastPush, held)
                                        : (seenDeviceBlock > 0 ? seenDeviceBlock : nominalBlockSamples);

    targetFillSamples = baseTargetSamples + (held > current ? held - current : 0);
}

void DeviceInputStream::resetInterpolator() noexcept
{
    previousSample = 0.0f;
    currentSample = 0.0f;
    phase = 0.0;
    primed = false;
    carrySample = 0.0f;
    carryPhase = 0.0;
    carryPending = false;
    heldLast = false;
}

void DeviceInputStream::countGap (int samples) noexcept
{
    if (samples <= 0)
        return;

    underruns.fetch_add (static_cast<uint64_t> (samples), std::memory_order_relaxed);

    // One gap, one event, however many pulls it spans.
    if (! inGap)
        lossEvents.fetch_add (1, std::memory_order_relaxed);

    inGap = true;
}

void DeviceInputStream::prepare (double sampleRate, int bufferSizeSamples)
{
    const auto block = static_cast<size_t> (std::max (1, bufferSizeSamples));

    // Storage for the largest block a device may actually deliver, not just
    // the one asked for; see kLargestDeviceBlock.
    ring.reset (std::max (block, static_cast<size_t> (kLargestDeviceBlock))
                * static_cast<size_t> (kRingBlocks));
    seenDeviceBlock = 0;
    alignmentSilence.store (0, std::memory_order_relaxed);
    nominalBlockSamples = block;
    largestPushSamples.store (0, std::memory_order_relaxed);
    previousPushSamples = 0;
    deviceBlockSamples.store (0, std::memory_order_relaxed);
    largestPullSamples.store (0, std::memory_order_relaxed);
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;

    // §5.4: this is monitor latency, so it is one pull and a few milliseconds
    // of late delivery rather than a fraction of the ring. The remaining
    // twelve or more blocks of capacity are headroom the loop never intends
    // to use: room for the driver's whole ring to land at once after a late
    // wake.
    targetFillSamples = cushionSamples (block, rate);
    baseTargetSamples = targetFillSamples;

    compensator = DriftCompensator (sampleRate);
    resetInterpolator();
    started.store (false, std::memory_order_relaxed);
    fillAverage = 0.0;
    fillAverageValid = false;
    silenceOwed = 0.0;
    pullsSinceSilence = 0;
    rebuffering = false;
    rebufferCounted = true;
    rebufferOwed = false;
    provisionalBlock = 0;
    inGap = false;

    driftPpm.store (0.0, std::memory_order_relaxed);
    excessDrift.store (false, std::memory_order_relaxed);
    driftReportingResetEpoch.store (0, std::memory_order_relaxed);
    excessDriftSeconds = 0.0;
    observedDriftReportingResetEpoch = 0;
    underruns.store (0, std::memory_order_relaxed);
    lossEvents.store (0, std::memory_order_relaxed);
    primes.store (0, std::memory_order_relaxed);
    holds.store (0, std::memory_order_relaxed);
    skips.store (0, std::memory_order_relaxed);

    lastPushNs.store (0, std::memory_order_relaxed);
    lastPushSamples.store (0, std::memory_order_relaxed);
    pushedSamples.store (0, std::memory_order_relaxed);
    lastPullNs.store (0, std::memory_order_relaxed);
    pulledSamples.store (0, std::memory_order_relaxed);
    resetMeasurementWindow();

    // Reset with its siblings. It was the one counter here that was not, so it
    // ran for the life of the stream while the sentence built from it -- "about
    // N seconds lost so far" -- describes the current take. Everything overrun
    // while merely monitoring, or during an earlier take, was added to this
    // take's figure.
    overrunSamples.store (0, std::memory_order_relaxed);

    publishPlaceInTime();
}

void DeviceInputStream::pushBlock (const float* samples, int numSamples) noexcept
{
    if (samples == nullptr || numSamples <= 0)
        return;

    // A channel written as silence is not consumed -- pull() returns before
    // reading -- so its ring filling up is not the consumer falling behind.
    // Counting it was: a macOS input whose IOProc paused long enough to be
    // called dead and then resumed ran up "sound is still being dropped" and
    // stepped the buffer ladder for the rest of the take, every figure blaming
    // the computer. Nothing is written either: setLive (true) restarts the
    // channel from an empty ring. Still counted as delivered and stamped, so
    // the device's own clock and its liveness stay visible.
    if (! channelLive.load (std::memory_order_relaxed))
    {
        pushedSamples.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);
        lastPushSamples.store (numSamples, std::memory_order_relaxed);
        lastPushNs.store (nowNs(), std::memory_order_release);
        lastDeliveryWallNs.store (steadyNowNs(), std::memory_order_release);
        return;
    }

    // The device's real IO size, which CoreAudio does not promise is the one
    // asked for. The usable part of the ring follows it up, so a device at
    // 1156 frames gets sixteen of its own blocks of headroom rather than a
    // ring smaller than one delivery.
    if (static_cast<size_t> (numSamples) > largestPushSamples.load (std::memory_order_relaxed))
        largestPushSamples.store (static_cast<size_t> (numSamples), std::memory_order_relaxed);

    // The device's IO block: a size two deliveries in a row have reached.
    if (const auto pair = std::min (previousPushSamples, static_cast<size_t> (numSamples));
        pair > deviceBlockSamples.load (std::memory_order_relaxed))
        deviceBlockSamples.store (pair, std::memory_order_relaxed);
    previousPushSamples = static_cast<size_t> (numSamples);

    // A full ring means the consumer is not keeping up. Dropping the newest
    // samples is the only lock-free option; the loop reacts by speeding this
    // device's playout back up. What is dropped is COUNTED: this used to
    // discard the return value, and a stalled consumer lost audio with every
    // counter on the screen still reading zero.
    const size_t usable = usableCapacity();
    const size_t buffered = ring.availableForRead();
    const size_t room = usable > buffered + 1 ? usable - buffered - 1 : 0;
    const auto written = ring.write (samples, std::min (room, static_cast<size_t> (numSamples)));

    if (written < static_cast<size_t> (numSamples))
    {
        overrunSamples.fetch_add (static_cast<uint64_t> (numSamples) - written, std::memory_order_relaxed);
        lossEvents.fetch_add (1, std::memory_order_relaxed);
    }

    // Delivered, whether or not it fit: the measurement wants the device's
    // clock, and a dropped sample was still a sample the device produced.
    pushedSamples.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);

    // Published after the write, so a consumer that sees this stamp also sees
    // the block behind it in the ring's own release/acquire.
    lastPushSamples.store (numSamples, std::memory_order_relaxed);
    lastPushNs.store (nowNs(), std::memory_order_release);
    lastDeliveryWallNs.store (steadyNowNs(), std::memory_order_release);
}

void DeviceInputStream::noteSamplesLostBeforeDelivery (int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    // Counted as produced, stamped as delivered: the measurement pairs the
    // count with the moment the device's clock had reached it. The block
    // size is left alone, so the loop's placement of the next pull within
    // the device's block is unchanged.
    pushedSamples.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);
    lastPushNs.store (nowNs(), std::memory_order_release);
    lastDeliveryWallNs.store (steadyNowNs(), std::memory_order_release);
}

bool DeviceInputStream::readOne (float& out) noexcept
{
    return ring.read (&out, 1) == 1;
}

double DeviceInputStream::virtualFillNow (size_t available) const noexcept
{
    // The ring level, read at a pull, only ever moves in whole device blocks:
    // a device 150 PPM slow lowers it by 7 samples a second, but the pull sees
    // the same number for nine seconds and then a step of 64. A loop driven by
    // that staircase either does nothing or sees a whole block of error at
    // once -- and one block, at 5 PPM per sample, was the 200 PPM clamp.
    //
    // So the pull is placed within the device's current block instead: how far
    // the device has got since its last delivery, from that delivery's
    // timestamp and its block's nominal duration, is audio the device has
    // captured but not yet handed over. The level with that fraction folded in
    // is continuous across the delivery, and it is what the loop steers.
    // Equivalently: the lowest level this pull could have seen had it landed
    // just before the next delivery, which is the cushion that actually
    // matters for an underrun.
    const auto pushNs = lastPushNs.load (std::memory_order_acquire);
    const auto pushSamples = lastPushSamples.load (std::memory_order_relaxed);

    double virtualFill = static_cast<double> (available);

    if (pushNs > 0 && pushSamples > 0 && virtualFillEnabled.load (std::memory_order_relaxed))
    {
        const double periodNs = static_cast<double> (pushSamples) * 1.0e9 / rate;
        const double elapsedNs = static_cast<double> (nowNs() - pushNs);
        const double fraction = std::clamp (elapsedNs / periodNs, 0.0, 1.0);

        virtualFill -= static_cast<double> (pushSamples) * (1.0 - fraction);
    }

    return virtualFill;
}

double DeviceInputStream::fillErrorNow (size_t available) noexcept
{
    const double virtualFill = virtualFillNow (available);

    // Smoothed a little: a delivery that lands late by a fraction of a block
    // reads as a dip until it arrives, and the loop should see the trend
    // rather than the tremor. Thirty-two blocks is 43 ms at 64/48k, nothing
    // against a loop whose slew takes seven seconds to cross 150 PPM.
    if (! fillAverageValid)
    {
        fillAverage = virtualFill;
        fillAverageValid = true;
    }
    else
    {
        fillAverage += (virtualFill - fillAverage) * kFillSmoothing;
    }

    return fillAverage - static_cast<double> (targetFillSamples);
}

void DeviceInputStream::noteSilence (int samples) noexcept
{
    // Capped at what the ring can hold above target, which is the most late
    // audio a burst could ever leave there to be skipped. Its storage, not
    // the part of it in use now: that follows the largest delivery so far,
    // and a backlog handed over in one larger piece raises it only as it
    // lands -- a gap of more than fourteen 64-sample blocks used to be owed
    // only in part, and the rest of the late audio stayed in the ring.
    const double cap = static_cast<double> (ring.capacity())
                     - static_cast<double> (targetFillSamples);
    silenceOwed = std::min (cap, silenceOwed + static_cast<double> (samples));
    pullsSinceSilence = 0;
}

void DeviceInputStream::skipLateAudio (int numSamples) noexcept
{
    // Measured on the de-quantized level, which is what the loop holds at
    // target; the raw level sits up to a block above it just after a delivery,
    // and that block is not late audio. And never into what this pull itself
    // is about to take: a ring that is chronically short -- pulls larger than
    // the target fill -- owes silence every block, and skipping ahead of a
    // pull that will run dry anyway only makes it run dry sooner.
    const auto available = ring.availableForRead();
    const double excess = std::min (virtualFillNow (available) - static_cast<double> (targetFillSamples),
                                    static_cast<double> (available) - static_cast<double> (numSamples + 1));

    // Supplied again. Give the burst a ring's worth of pulls to land, then
    // write the silence off, skipped or not: what is still owed after that
    // is not coming.
    if (available > 0 && ++pullsSinceSilence > kRingBlocks)
    {
        silenceOwed = 0.0;
        return;
    }

    // A burst is whole blocks by nature. Anything under one is the loop's
    // own jitter around its target, and skipping it -- a few samples, with
    // the interpolator restarted each time -- put a click in every block
    // for as long as the owed silence lasted, which, since only a pull that
    // skipped nothing counted towards writing it off, was indefinitely.
    const double block = static_cast<double> (std::max (1, lastPushSamples.load (std::memory_order_relaxed)));

    if (excess >= block)
    {
        const auto skip = static_cast<size_t> (std::min (silenceOwed, excess));
        const auto skipped = ring.discard (skip);
        skips.fetch_add (1, std::memory_order_relaxed);

        // A burst that lands between two of the device's own periods leaves
        // the placement above short by up to a block until the next period
        // arrives; what is still owed after that waits for it.
        silenceOwed = std::max (0.0, silenceOwed - static_cast<double> (skipped));

        // The interpolator's pair predates the gap; the level's average was
        // taken while the ring was dry. Both restart from what is there now.
        resetInterpolator();
        fillAverageValid = false;
    }
}

void DeviceInputStream::pull (float* destination, int numSamples) noexcept
{
    if (destination == nullptr || numSamples <= 0)
        return;

    if (! channelLive.load (std::memory_order_relaxed))
    {
        // §6.5: the channel survives the mic leaving, and yields silence.
        std::fill (destination, destination + numSamples, 0.0f);
        return;
    }

    // §6.5 reconnection. Nothing consumed this stream while the channel was
    // dead, so everything here still describes the instant the microphone left:
    // a ring holding pre-gap audio, an interpolator holding the last sample
    // before it, a loop whose fill error is meaningless, and started/primed
    // both saying the stream is running.
    //
    // Resuming from that puts audio from before the gap after it -- or, when
    // the ring is dry because the old stream died with the device, holds that
    // last sample as a DC offset for the rest of the take while counting every
    // block as lost audio. Both are worse than the silence they replace, and
    // §0.1 makes the second one a false alarm about the one failure this app
    // promises not to have.
    //
    // So the channel restarts as if the stream had just opened: pre-roll again,
    // stay silent until it is buffered, and only then consume. Real-time safe --
    // a read-index store, a few scalars, no allocation (§11).
    if (restartPending.exchange (false, std::memory_order_acquire))
    {
        ring.clear();
        compensator.reset();

        resetInterpolator();
        started.store (false, std::memory_order_relaxed);
        fillAverageValid = false;
        silenceOwed = 0.0;
        pullsSinceSilence = 0;
        rebuffering = false;
        rebufferOwed = false;
        provisionalBlock = 0;
        inGap = false;

        driftPpm.store (0.0, std::memory_order_relaxed);
        excessDrift.store (false, std::memory_order_relaxed);
        driftReportingResetEpoch.fetch_add (1, std::memory_order_release);
    }

    // The device's IO size, first. A block larger than any this stream has
    // held room for means the device now keeps its audio that much longer
    // before handing it over, so the channel has to run that much later: the
    // ring has already run dry for most of the difference by the time the
    // block lands -- the gap no buffering could have avoided, counted as it
    // happened -- and whatever is still short of the new target is buffered
    // here, once and exactly, rather than left to the slew-limited loop with
    // no cushion at all. The silence already written stood in for the delay, not
    // for late audio, so none of the block is skipped. A block no larger than
    // what is held (a device going back to a size it ran at before) changes
    // nothing at all: the headroom was kept for it.
    const auto lastPush = static_cast<size_t> (std::max (0, lastPushSamples.load (std::memory_order_acquire)));
    {
        const auto block = deviceBlockSamples.load (std::memory_order_relaxed);
        const bool firstDelivery = seenDeviceBlock == 0;
        const size_t before = heldLatency(); // a provisional block's room included

        // A provisional block (below) is settled by the delivery after it.
        if (provisionalBlock > 0)
        {
            if (block >= provisionalBlock)
            {
                // Confirmed. The gap was the device holding its audio longer,
                // and the channel was moved for it then: nothing in the ring
                // is late, and none of it is skipped.
                provisionalBlock = 0;
                silenceOwed = 0.0;
                rebufferOwed = false;
            }
            else if (lastPush < provisionalBlock)
            {
                // Refused: the next delivery is back at the device's own size,
                // so the large one was a backlog handed over in one piece and
                // the gap was audio arriving late after all. The silence
                // written for it -- the gap and the buffering up for a block
                // that is not coming -- is still owed, and the late audio
                // standing in the ring for it is skipped (skipLateAudio) down
                // to the target the device's real block sets. Holding it
                // instead kept the channel a block behind every other stem
                // for minutes while the loop drained it at 200 PPM.
                provisionalBlock = 0;
                pullsSinceSilence = 0;
            }
        }

        if (block > seenDeviceBlock)
        {
            seenDeviceBlock = block;

            if (! firstDelivery && started.load (std::memory_order_relaxed)
                && heldLatency() > before)
            {
                // Counted as loss only as part of a gap the ring really ran dry
                // for (the branch below, or one still being buffered). A growth
                // the cushion covered lost nothing: the silence that moves the
                // channel to its new place is a shift
                // (getAlignmentSilenceSamples), not an underrun that would step
                // the buffer ladder and tell the user audio was lost.
                rebufferCounted = (rebuffering && rebufferCounted) || inGap;
                rebuffering = true;
                rebufferOwed = false;
                silenceOwed = 0.0;
            }
        }
    }

    // The same change, seen one delivery sooner: the ring ran dry and what
    // ended the gap was a block larger than any held room for. If the device
    // has grown, the gap was it holding its audio longer, not audio arriving
    // late, and the stream buffers up to its target now rather than running a
    // device period with no cushion at all. But one large delivery is also
    // what a driver handing over a backlog looks like, so the block is only
    // provisional until the next delivery confirms or refuses it (above):
    // held as room meanwhile, and the silence written for it still owed, so a
    // refusal can take it back.
    if (inGap && started.load (std::memory_order_relaxed) && lastPush > heldLatency())
    {
        provisionalBlock = lastPush;
        rebuffering = true;
        rebufferCounted = true;
        rebufferOwed = true;
    }

    // Where this stream now sits, for the writer to line its stem up by.
    publishPlaceInTime();

    // The output's real callback size, which CoreAudio does not promise is
    // the one asked for either. The cushion is sized from the pull, not the
    // nominal block (cushionSamples): held at 128 samples, an
    // output taking 1024 at a time ran the ring dry in every callback.
    if (static_cast<size_t> (numSamples) > largestPullSamples.load (std::memory_order_relaxed))
    {
        largestPullSamples.store (static_cast<size_t> (numSamples), std::memory_order_relaxed);

        // A first pull lands during pre-roll, so the target is right before
        // the stream starts. A larger one after that -- the software clock
        // pulled at the nominal size until the output's first, bigger
        // callback arrived -- finds a ring that cannot serve even this pull,
        // and the loop, at 200 PPM, would take minutes to build the level up:
        // a dry patch in every callback until then. So the stream buffers up
        // to the new target once, in one gap, and that gap is counted.
        if (started.load (std::memory_order_relaxed)
            && ring.availableForRead() < static_cast<size_t> (numSamples) + nominalBlockSamples)
        {
            rebuffering = true;
            rebufferCounted = true;
        }
    }

    // Every pull: the target follows the device's current block (see
    // recomputeTarget), which can change with any delivery.
    recomputeTarget();

    if (rebuffering)
    {
        // Judged on the de-quantized level the loop steers, like pre-roll:
        // just after a large delivery the raw level is the whole block, and
        // a whole device period stands between it and the next one.
        const double deficit = static_cast<double> (targetFillSamples)
                             - virtualFillNow (ring.availableForRead());

        if (deficit >= 1.0)
        {
            // Exactly the shortfall, not whole pulls of it: the channel lands
            // on its new place in time rather than up to a pull past it.
            const auto silent = static_cast<int> (std::min (static_cast<double> (numSamples), std::ceil (deficit)));

            // The consumer's clock still ran: counted and stamped, so the
            // drift measurement does not read the gap as a slow output.
            pulledSamples.fetch_add (static_cast<uint64_t> (silent), std::memory_order_relaxed);
            lastPullNs.store (nowNs(), std::memory_order_release);

            std::fill (destination, destination + silent, 0.0f);

            if (rebufferCounted)
                countGap (silent);
            else
                alignmentSilence.fetch_add (static_cast<uint64_t> (silent), std::memory_order_relaxed);

            // Buffering up across a gap whose cause is not settled yet: like
            // the gap's own silence, it is answered by late audio if late
            // audio is what turns up.
            if (rebufferOwed)
                noteSilence (silent);

            if (silent == numSamples)
                return;

            destination += silent;
            numSamples -= silent;
        }

        rebuffering = false;
        rebufferOwed = false;
        fillAverageValid = false;
    }

    // Pre-roll. The output clock starts before any device has delivered, so
    // consuming here would emit a click at the top of every take and count
    // audio as lost that had simply not arrived yet.
    //
    // Judged on the de-quantized level the loop steers, not the raw one. Just
    // after a 1156-frame delivery the raw level is the whole block, far past
    // the target at 64, but the device's next block is a whole period away:
    // starting there ran the ring dry a few dozen samples before it landed --
    // a counted dropout at the top of every stream on such a device -- and
    // left the loop starting a full block below its target.
    if (! started.load (std::memory_order_relaxed))
    {
        if (virtualFillNow (ring.availableForRead()) < static_cast<double> (targetFillSamples))
        {
            std::fill (destination, destination + numSamples, 0.0f);
            return;
        }

        // Start exactly at the target, not past it. The level at the first
        // pull is where this channel sits in time against every other one,
        // and the loop moves it only slowly: starting a pull deep left a
        // channel up to a block behind its neighbours for a minute, and its
        // place in time -- which the writer lines the stems up by -- only as
        // accurate as the pull that happened to cross the target. What is
        // dropped is pre-roll, before anything has been played or recorded
        // from this stream: audio from before the moment the channel's first
        // sample stands for, which the take already holds as silence.
        //
        // All of the overshoot, however large. Only a surplus smaller than a
        // pull used to go; a bigger one was left to the loop. At 480 or 512
        // samples two deliveries landing between two pulls overshoot by a
        // whole block, and the loop, at its clamp, took a minute and a half
        // to drain it: that microphone's headphone delay swept 19 ms against
        // the others meanwhile -- a slow flange wherever their sound bleeds
        // into each other -- and its ring dipped to 45 samples on the way.
        if (const auto excess = static_cast<size_t> (std::max (
                0.0, std::floor (virtualFillNow (ring.availableForRead())
                                 - static_cast<double> (targetFillSamples))));
            excess > 0)
            ring.discard (excess);

        started.store (true, std::memory_order_relaxed);
    }

    // Count, then stamp: a reporter that reads the stamp sees a count at most
    // one block newer than it, never older.
    pulledSamples.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);
    lastPullNs.store (nowNs(), std::memory_order_release);

    // §3.2: fill error drives the loop. Positive means this device is producing
    // faster than this stream is being consumed, so its ratio must rise to
    // drain it.
    //
    // Every channel is steered, the clock master included. The thing this
    // stream is pulled by is the output device's callback, not any microphone,
    // so exempting one mic from correction does not make it the timebase -- it
    // just leaves that one mic uncorrected against a clock it has no
    // relationship to. Its ring then walks to one end of its travel and stays
    // there, dropping arrivals when full or holding the last sample when dry,
    // which is drift on the one channel §3.1 nominated as the reference. See
    // §3.1/§3.2 in docs/SPEC.md for why the reference is a reporting role here
    // rather than a correction one.
    //
    // Audio arriving late for a span already written as silence is skipped
    // before the level is read, so the loop never sees it as fill.
    if (silenceOwed > 0.0)
        skipLateAudio (numSamples);

    const double fillError = fillErrorNow (ring.availableForRead());

    compensator.update (fillError, numSamples);
    driftPpm.store (compensator.getPpm(), std::memory_order_relaxed);

    const double ratio = compensator.getRatio();

    if (! primed)
    {
        // A sample carried over a gap is primed with only once the audio
        // after it is there too; on its own it would play and the pull run
        // dry one sample later, a gap split in two.
        const bool primedFromCarry = carryPending && ring.availableForRead() > 0;

        if (primedFromCarry)
        {
            // The pair and the phase the gap interrupted, exactly: the carried
            // sample and the one after it, the output landing between them
            // where the next one would have. Priming on the carried sample
            // alone -- both ends of the pair the same sample, phase zero --
            // played it twice and dropped the fraction of a sample the
            // resampler had reached, so the channel came back up to two
            // samples later than the silence written for the gap: its stem
            // out of step with every other by that much after each gap.
            previousSample = carrySample;
            readOne (currentSample); // there is one: checked above
            phase = carryPhase;
            carryPending = false;
        }
        else if (! readOne (currentSample))
        {
            std::fill (destination, destination + numSamples, 0.0f);
            countGap (numSamples);
            noteSilence (numSamples);
            return;
        }
        else
        {
            previousSample = currentSample;
            phase = 0.0;
        }

        primed = true;
        primes.fetch_add (1, std::memory_order_relaxed);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        // Advance to the pair this output sample lies between BEFORE writing
        // it, never after. Reading ahead after the last sample of a block
        // meant a block that consumed the ring exactly -- every source sample
        // used, none to spare -- ended in a failed read, and the interpolator
        // was zeroed and re-primed for a starvation that had not happened:
        // its fractional phase snapped to zero, a sub-sample step in the
        // output, once per block in a ring held one block from empty.
        while (phase >= 1.0)
        {
            previousSample = currentSample;

            if (! readOne (currentSample))
            {
                // Only the sample this block's LAST output would have leaned
                // on is missing: the ring held exactly this block's worth,
                // and the pair the interpolator carries between blocks needs
                // one more. Hold the sample it has for that one output and
                // keep the pair, with the crossing still pending, so the next
                // block's first read completes it. De-priming here was a
                // trap: the next block primed again, which costs the same
                // extra sample, ran one short again, and so on -- a sample
                // of silence and a restart in every block for as long as the
                // ring sat one block deep, which the loop, at 200 PPM, took
                // half a minute to lift it out of. One held sample, once,
                // against a step to zero and a phase reset every block.
                if (i == numSamples - 1 && i > 0)
                {
                    currentSample = previousSample;
                    destination[i] = previousSample;
                    holds.fetch_add (1, std::memory_order_relaxed);
                    heldLast = true;
                    inGap = false;
                    return;
                }

                // Nothing left to interpolate towards. The source did not
                // provide the remainder of this block, so write silence for
                // exactly that missing span, count it once, and stop. Holding
                // the last non-zero sample here manufactures a DC signal that
                // was never captured; on a sustained starvation that offset is
                // both audible and unsafe in the monitor path.
                //
                // The previous version broke out of the inner loop and let the
                // outer one continue, which re-entered here on the very next
                // sample -- the ring was still dry -- and added (numSamples - i)
                // again each time. A single dry 64-sample block was reported as
                // ~2000 lost samples instead of the few dozen that were actually
                // missing. §0.1 makes any non-zero underrun the failure the user
                // is shown, so an inflated count is a false alarm about the one
                // thing this app promises not to do.
                // The next pull must not reuse the sample we just emitted.
                // Leaving the interpolator primed makes every later dry block
                // begin with that stale value before discovering the empty
                // ring, producing one click and under-counting the loss by one
                // frame per block.
                //
                // What was just read and not yet played -- the target the last
                // output leaned towards -- is not lost with the pair: the next
                // prime starts from it, so the audio after the gap carries on
                // from the sample after the last one heard. Unless a held
                // sample already played it.
                const bool carry = ! heldLast;
                const float carried = previousSample;

                // And how far past it this output had got: the resampler's
                // fraction of a sample, which the output after the gap picks
                // up from rather than from zero.
                const double carriedPhase = std::clamp (phase - 1.0, 0.0, 0.999999);
                resetInterpolator();

                if (carry)
                {
                    carrySample = carried;
                    carryPhase = carriedPhase;
                    carryPending = true;
                }

                const int remaining = numSamples - i;

                std::fill (destination + i, destination + numSamples, 0.0f);

                // A pull that played something before running dry starts a
                // new gap; one that played nothing continues the last.
                if (i > 0)
                    inGap = false;

                countGap (remaining);

                // Exactly the silence written, no more: the sample this read
                // did not get is the one the next pull primes with, and it
                // comes out where it would have.
                noteSilence (remaining);
                return;
            }

            heldLast = false;
            phase -= 1.0;
        }

        destination[i] = previousSample
                         + static_cast<float> (phase) * (currentSample - previousSample);

        phase += ratio;
    }

    inGap = false;
}

void DeviceInputStream::resetMeasurementWindow() noexcept
{
    windowStart = 0;
    windowCount = 0;
    measured.store (false, std::memory_order_relaxed);
    measuredPpm.store (0.0, std::memory_order_relaxed);
    measurementSeconds.store (0.0, std::memory_order_relaxed);
    deviceRatePpm.store (0.0, std::memory_order_relaxed);
    consumerRatePpm.store (0.0, std::memory_order_relaxed);
}

namespace {

// Least-squares slope of y against x over a ring of points; false when the
// points do not spread in x. Two passes, so a 60-second window of sample
// counts near 3e6 keeps its precision.
template <typename Point, typename GetX, typename GetY>
bool slopeOf (const Point* ring, int start, int count, int capacity, GetX getX, GetY getY, double& slope)
{
    double meanX = 0.0, meanY = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const auto& p = ring[(start + i) % capacity];
        meanX += getX (p);
        meanY += getY (p);
    }
    meanX /= count;
    meanY /= count;

    double sxy = 0.0, sxx = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const auto& p = ring[(start + i) % capacity];
        const double dx = getX (p) - meanX;
        sxy += dx * (getY (p) - meanY);
        sxx += dx * dx;
    }

    if (sxx <= 0.0)
        return false;

    slope = sxy / sxx;
    return true;
}

} // namespace

void DeviceInputStream::tickDriftReporting (double elapsedSeconds, double referencePpm) noexcept
{
    // §3.3 judges a device against the clock master, not against the output
    // stream. Passing the master's own measurement as the reference is what
    // keeps a skewed *output* device from flagging every microphone at once:
    // that skew lands in every channel's figure equally and subtracts out here.
    const auto resetEpoch = driftReportingResetEpoch.load (std::memory_order_acquire);
    if (! channelLive.load (std::memory_order_relaxed)
        || resetEpoch != observedDriftReportingResetEpoch)
    {
        observedDriftReportingResetEpoch = resetEpoch;
        excessDriftSeconds = 0.0;
        excessDrift.store (false, std::memory_order_relaxed);
        resetMeasurementWindow();
        return;
    }

    // The measurement. Each tick records, for each side, its sample count
    // paired with the timestamp of the block that brought it there; the slope
    // of count against time is that clock's rate. The device's rate over the
    // consumer's is the figure. A count and its stamp are two atomics, so a
    // tick can see a count one block newer than its stamp: a block of noise
    // per point, white, which a fit over hundreds of points averages down --
    // a difference of two points would carry it whole, 40 PPM at a minute.
    if (! started.load (std::memory_order_relaxed))
        return;

    const auto pushNs = lastPushNs.load (std::memory_order_acquire);
    const auto pullNs = lastPullNs.load (std::memory_order_acquire);

    if (pushNs <= 0 || pullNs <= 0)
        return;

    const RatePoint point { static_cast<double> (pushNs) * 1.0e-9,
                            static_cast<double> (pushedSamples.load (std::memory_order_relaxed)),
                            static_cast<double> (pullNs) * 1.0e-9,
                            static_cast<double> (pulledSamples.load (std::memory_order_relaxed)) };

    if (windowCount == kMaxWindowPoints)
    {
        windowStart = (windowStart + 1) % kMaxWindowPoints;
        --windowCount;
    }

    window[(windowStart + windowCount) % kMaxWindowPoints] = point;
    ++windowCount;

    // Drop what has aged out of the window, keeping a little more than the
    // named length so the fit always spans it.
    while (windowCount > 2
           && point.pullSeconds - window[windowStart].pullSeconds > kMeasurementSeconds * 1.25)
    {
        windowStart = (windowStart + 1) % kMaxWindowPoints;
        --windowCount;
    }

    const double span = point.pullSeconds - window[windowStart].pullSeconds;
    measurementSeconds.store (span, std::memory_order_relaxed);

    if (windowCount >= 8 && span >= kMeasurementSeconds)
    {
        double deviceRate = 0.0, consumerRate = 0.0;

        const bool ok = slopeOf (window, windowStart, windowCount, kMaxWindowPoints,
                                 [] (const RatePoint& p) { return p.pushSeconds; },
                                 [] (const RatePoint& p) { return p.pushed; }, deviceRate)
                     && slopeOf (window, windowStart, windowCount, kMaxWindowPoints,
                                 [] (const RatePoint& p) { return p.pullSeconds; },
                                 [] (const RatePoint& p) { return p.pulled; }, consumerRate)
                     && consumerRate > 0.0;

        if (ok)
        {
            measuredPpm.store ((deviceRate / consumerRate - 1.0) * 1.0e6, std::memory_order_relaxed);
            deviceRatePpm.store ((deviceRate / rate - 1.0) * 1.0e6, std::memory_order_relaxed);
            consumerRatePpm.store ((consumerRate / rate - 1.0) * 1.0e6, std::memory_order_relaxed);
            measured.store (true, std::memory_order_relaxed);
        }
    }

    // §3.3's flag, from the measurement, once there is one. Before that there
    // is nothing honest to flag: the loop's own figure is still settling.
    if (! measured.load (std::memory_order_relaxed))
    {
        excessDriftSeconds = 0.0;
        excessDrift.store (false, std::memory_order_relaxed);
        return;
    }

    const double relativePpm = measuredPpm.load (std::memory_order_relaxed) - referencePpm;

    if (std::abs (relativePpm) > kExcessDriftThresholdPpm)
    {
        excessDriftSeconds += std::max (0.0, elapsedSeconds);
        if (excessDriftSeconds >= kExcessDriftSustainSeconds)
            excessDrift.store (true, std::memory_order_relaxed);
    }
    else
    {
        excessDriftSeconds = 0.0;
        excessDrift.store (false, std::memory_order_relaxed);
    }
}

double DeviceInputStream::getFillFraction() const noexcept
{
    const auto usable = usableCapacity();
    return usable > 1 ? std::min (1.0, static_cast<double> (ring.availableForRead())
                                           / static_cast<double> (usable - 1))
                      : 0.0;
}

bool DeviceInputStream::deliveredWithin (int64_t windowNs) const noexcept
{
    const auto stamped = lastDeliveryWallNs.load (std::memory_order_acquire);
    return stamped != 0 && steadyNowNs() - stamped <= windowNs;
}

} // namespace mma
