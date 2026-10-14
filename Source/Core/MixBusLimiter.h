#pragma once
#include <cstdint>
#include "PeakLimiter.h"

namespace mma {

/// §6.1: the recorded MIX.wav file's limiter. A separate instance from
/// MonitorBus's safety limiter -- this one is never affected by the monitor's
/// runaway cut or global mute, and the monitor is never affected by this one.
/// Ceiling -1dBFS; no runaway/mute semantics apply to the recording path.
///
/// It turns a peak down rather than squaring it off (PeakLimiter: instant
/// attack, 20 ms hold, 50 ms release, the ceiling still guaranteed). It used to
/// be a per-sample hard clip, and MIX.wav is summed at unity from every
/// microphone in one room, so a few overlapping voices flat-topped the file
/// every time they peaked together -- the one file a novice plays back.
/// One instance per take, owned by the writer; the stems never pass through it.
class MixBusLimiter
{
public:
    static constexpr float kCeilingDb = -1.0f;

    explicit MixBusLimiter (double sampleRate = 48000.0) noexcept;

    /// The trimmed, summed mix-file sample in, the sample to write out. A NaN
    /// or infinity becomes silence. Not thread-safe: the writer thread only.
    float processSample (float sumInputSample) noexcept;

    /// Times the limiter has started turning the mix down from unity.
    uint64_t getEngagementCount() const noexcept { return limiter.getEngagements(); }

    float getCeilingLinear() const noexcept { return limiter.getCeiling(); }

private:
    PeakLimiter limiter;
};

} // namespace mma
