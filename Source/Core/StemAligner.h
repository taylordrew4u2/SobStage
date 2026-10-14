#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mma {

/// Why a channel's offset changed, as far as its own device is concerned.
///  - settled: the device's IO block is known (two deliveries at it), or
///    the change is another device's doing.
///  - provisional: the channel's own stream has just moved for a block one
///    large delivery suggested and the device has not yet confirmed. It may
///    be a driver handing over a backlog in one piece instead.
///  - refused: the device's next delivery said it was a backlog, and the
///    stream has gone back to its own block. This takes back the last
///    provisional change; it is not a move of its own.
///  - released: the channel's own stream has given back room it held for a
///    block its device has left, by dropping that much quiet from the
///    newest end of what it had buffered (DeviceInputStream). The change
///    carries how much (StemOffsetEvent::released): that part of it is a
///    skip at the newest sample the channel has handed over, not at the
///    oldest one waiting in its line.
enum class StemOffsetKind : uint8_t
{
    settled,
    provisional,
    refused,
    released
};

/// One channel's alignment changing: from `frame` of the take on (counted in
/// frames the writer accepted), channel `channel` is to be held back by
/// `offset` samples. For a released change, `released` is how many samples
/// of it the channel's own stream skipped at that frame; any more of the
/// change is another device's doing.
struct StemOffsetEvent
{
    uint64_t frame = 0;
    int channel = 0;
    int offset = 0;
    StemOffsetKind kind = StemOffsetKind::settled;
    int released = 0;
};

/// Carries alignment changes from the audio thread to the writer thread,
/// beside the audio itself. Single producer (the callback handing blocks to
/// WritePipeline), single consumer (the writer). Fixed storage, no locks and
/// no allocation on either side (§11).
class StemOffsetQueue
{
public:
    /// Changes come only when a device's IO size or the rig's slowest device
    /// changes -- a handful a take -- so this is far more than is ever queued.
    static constexpr size_t kCapacity = 1024;

    /// Not concurrent with push or pop: before the take's audio starts.
    void clear() noexcept;

    /// Producer. False when full; nothing is queued then.
    bool push (const StemOffsetEvent& event) noexcept;

    /// Consumer: the oldest change, without taking it.
    bool peek (StemOffsetEvent& event) const noexcept;

    /// Consumer: takes the oldest change. Only after a successful peek.
    void pop() noexcept;

private:
    std::array<StemOffsetEvent, kCapacity> slots {};
    std::atomic<size_t> head { 0 }; // next slot the producer writes
    std::atomic<size_t> tail { 0 }; // next slot the consumer reads
};

/// Lines the stems up on the writer thread, where it costs the headphones
/// nothing (§5.4, §6.1).
///
/// Every microphone reaches the headphone mix as early as its own device
/// allows: a device that reports more input latency, or runs at a larger IO
/// block, hands its audio over later, and nothing on the monitor path waits
/// for it. So the samples the writer receives are not lined up -- a clap two
/// interfaces heard at once arrives in their channels that many samples
/// apart. Each channel is delayed here by its offset (the slowest device's
/// latency less its own), through a delay line per channel, so the clap lands
/// on the same frame in every stem and the mix summed from them.
///
/// A channel's offset can change mid-take, when a device's IO block grows:
///  - Longer: the channel is held back further by writing silence into it
///    at once while its audio waits in the delay line -- nothing it handed
///    over is lost, it comes out later. That is where another device's
///    growth belongs: the slowest device's stream moved later at the moment
///    this channel's oldest waiting sample stands for.
///  - Longer, for room the channel's own stream gave back (released): the
///    stream skipped a moment of quiet at the newest sample it handed over,
///    so the silence goes there, at the newest end of the line, and what
///    was already waiting comes out on time. Written at the head instead,
///    it landed as many samples before the quiet moment as the channel was
///    held back, inside whatever sound was there, and those waiting samples
///    came out that much late against every other stem: a 23 ms dropout in
///    the middle of a word.
///  - Shorter: the channel's own device has grown its block and its stream
///    has just moved later by putting exactly that much silence in
///    (DeviceInputStream moves once, by the growth). That silence is what
///    is taken out: the run of it already waiting at the newest end of the
///    line, then whatever of it is still arriving. Only what neither finds
///    -- audio arrived first -- comes out of the newest samples waiting, so
///    a growth on a device that is not the slowest leaves its stem
///    seamless.
///  - A shorter offset for a block the device has not confirmed is applied
///    the same way, at once, while the move's silence is still at the newest
///    end of the line -- and remembered. When the device refuses that block
///    (a driver handed a backlog over in one piece, and the stream moved
///    nothing in the end), the longer offset that follows puts that silence
///    back exactly where it was taken from, behind the samples that were
///    waiting ahead of it, and neither change is counted: the stem is as if
///    nothing had happened. Writing fresh silence at the head instead put
///    the audio still waiting from before the backlog's gap after it.
///  - The offset set before a channel's first sample is the take's starting
///    alignment: the leading channels start with that much silence.
///
/// What is still in a line when the take stops is audio from after the
/// slowest device's last sample, past the aligned end of the take, and is not
/// written: every stem holds exactly the frames the take accepted.
///
/// Memory is allocated once, in prepare(), and bounded by kMaxOffsetSamples a
/// channel. Not real-time: process() and setOffset() run on the writer thread
/// and are cheap enough never to hold it up. The getters are for any thread.
class StemAligner
{
public:
    /// The most a channel is ever held back: a quarter of a second at 96 kHz
    /// of input latency plus the largest IO block a device can run at, far
    /// past any wired interface's figures. An offset past it is clamped, and
    /// the take's alignment then reported as not exact. 112 KB a channel.
    static constexpr int kMaxOffsetSamples = 24000 + 4096;

    /// Writer side, before the take: allocates every channel's line.
    void prepare (int numChannels);

    /// Writer thread: from the next sample process() takes for `channel`, it
    /// is held back by `samples`. `kind` says whether the change is for a
    /// block the device has not confirmed yet, takes such a change back, or
    /// is room the channel's own stream gave back -- `released` samples of
    /// it, skipped at the newest sample handed over.
    void setOffset (int channel, int samples, StemOffsetKind kind = StemOffsetKind::settled,
                    int released = 0) noexcept;

    /// Writer thread: `frames` samples of one channel, in place, through its
    /// line.
    void process (int channel, float* samples, size_t frames) noexcept;

    int getNumChannels() const noexcept { return static_cast<int> (lines.size()); }

    /// The offset in force now, and the one the take started with. Until the
    /// channel's first sample has gone through, the starting offset is the
    /// one already set for it -- the silence its stem will open with -- not
    /// zero: the writer reaches a channel some milliseconds after the take
    /// begins, and a record written in between must not say the stem starts
    /// unshifted while its offset says otherwise.
    int getOffset (int channel) const noexcept;
    int getStartOffset (int channel) const noexcept;

    /// Whether the channel's first sample has gone through, which is when its
    /// starting offset is fixed.
    bool hasStarted (int channel) const noexcept;

    /// Silence written into this channel after its first sample to hold it
    /// back further, and samples taken out to bring it forward.
    uint64_t getSilenceInserted (int channel) const noexcept;
    uint64_t getSamplesDropped (int channel) const noexcept;

    /// False once any offset had to be clamped.
    bool isExact() const noexcept { return exact.load (std::memory_order_relaxed); }

private:
    struct Line
    {
        std::vector<float> buffer;
        size_t head = 0;  // oldest sample waiting
        size_t size = 0;  // samples waiting
        size_t paddingOwed = 0; // silence still to write for the starting offset
        size_t silenceOwed = 0; // silence still to write for a later, longer one
        size_t skipOwed = 0;    // silence still to take out as it arrives, for a shorter one
        bool started = false;

        // The last provisional shorter offset, kept so a refusal can undo it:
        // the silence it took out (from the newest end of the line, then as
        // it arrived), how many samples were waiting ahead of that silence,
        // and how many have gone out of the line since. Invalid once anything
        // else changes the offset, or audio had to be taken out for it.
        struct Undo
        {
            bool valid = false;
            size_t silence = 0;
            size_t ahead = 0;
            size_t emitted = 0;
        } undo;

        std::atomic<int> offset { 0 };
        std::atomic<int> startOffset { -1 }; // -1 until the first sample
        std::atomic<uint64_t> silenceInserted { 0 };
        std::atomic<uint64_t> dropped { 0 };
    };

    std::vector<std::unique_ptr<Line>> lines;
    std::atomic<bool> exact { true };

    static void dropNewest (Line& line, size_t count) noexcept;
    static size_t insertSilence (Line& line, size_t position, size_t count) noexcept;
};

} // namespace mma
