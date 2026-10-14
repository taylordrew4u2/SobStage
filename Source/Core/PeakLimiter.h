#pragma once
#include <cstdint>

namespace mma {

/// A zero-lookahead peak limiter that turns the signal down instead of
/// squaring it off.
///
/// Both of the app's limiters used to be a per-sample hard clip: any sample
/// over the ceiling was replaced by the ceiling. On speech that is a
/// flat-topped peak every time a few voices overlap, heard as crackle; on a
/// sustained vowel that six microphones in one room all hear, the headphones
/// spent a third of the time flat on the ceiling. This one reacts on the same
/// sample (instant attack, so nothing gets past the ceiling and nothing has
/// to be delayed to look ahead), holds the reduced gain for 20 ms -- for as
/// long as the signal stays within 1 dB of the level that set it -- so it does
/// not ripple inside one cycle of a low voice, then lets it back to unity
/// with a 50 ms time constant. A loud moment is turned down for a fraction of
/// a second instead of being bent out of shape: only the rise of its first
/// crest, which nothing without lookahead can see coming, still meets the
/// ceiling flat (that vowel: 12 samples in a minute).
///
/// Why 20 ms. A voice has one main crest per pitch period, and only the
/// samples near it are loud enough to renew the hold, so the hold has to
/// outlast the gap from one crest to the next. It was 10 ms, which only
/// covers voices above about 95 Hz: an 80 Hz voice (12.5 ms between crests)
/// had the gain let go and pulled back down once per period, squaring off
/// the rise of every crest -- 0.27 dB of ripple, and 1630 flat-topped samples
/// in 20 s of one vowel heard by six microphones. 20 ms covers fundamentals
/// down to 50 Hz, below any speaking voice.
///
/// The ceiling is still guaranteed: the output is clamped to it as a final
/// backstop, which only ever acts on the rounding in the gain it just set.
///
/// Real-time safe: no allocation, no locks, a division only on a sample that
/// has to be turned down further.
class PeakLimiter
{
public:
    static constexpr double kHoldSeconds = 0.020;
    static constexpr double kReleaseSeconds = 0.050;

    PeakLimiter() noexcept : PeakLimiter (48000.0, 1.0f) {}
    PeakLimiter (double sampleRate, float ceilingLinear) noexcept;

    /// One sample in, one out, never above the ceiling. A NaN or infinity
    /// comes out as silence and leaves the limiter as it was: a device fault
    /// is not a loud signal, and must not hold the gain down afterwards.
    float process (float sample) noexcept;

    /// Whether the last sample processed was over the ceiling on its own, at
    /// unity gain -- the limiter had to act on it. Over-ceiling samples in a
    /// row are what "engaged" means to a caller timing a runaway.
    bool wasOverCeiling() const noexcept { return overCeiling; }

    /// Times the limiter has started turning the signal down from unity.
    /// One loud passage is one engagement however many peaks it has; the
    /// next one counts once the gain has come all the way back.
    uint64_t getEngagements() const noexcept { return engagements; }

    /// The gain applied to the last sample, 1 when nothing is being turned down.
    float getGain() const noexcept { return gain; }

    float getCeiling() const noexcept { return ceiling; }

    /// Back to unity gain, as if nothing loud had ever been heard. The
    /// engagement count is kept: it is a history, not state.
    void reset() noexcept;

private:
    float ceiling = 1.0f;
    float releaseCoefficient = 0.0f;
    int holdSamples = 0;
    int holdRemaining = 0;
    float gain = 1.0f;
    bool overCeiling = false;
    uint64_t engagements = 0;
};

} // namespace mma
