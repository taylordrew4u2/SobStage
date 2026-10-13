#include "MixBusLimiter.h"
#include <cmath>

namespace mma {

MixBusLimiter::MixBusLimiter (double sampleRate) noexcept
    // Computed once per take rather than on every sample. The writer thread
    // runs processSample over every frame of every take, and a std::pow whose
    // operands never change had no business being inside that loop.
    : limiter (sampleRate, std::pow (10.0f, kCeilingDb / 20.0f))
{
}

float MixBusLimiter::processSample (float sumInputSample) noexcept
{
    // A NaN fails every comparison, so without this it passed the ceiling
    // check and went into the file as whatever the integer conversion made
    // of it. PeakLimiter turns it into silence without disturbing its gain.
    return limiter.process (sumInputSample);
}

} // namespace mma
