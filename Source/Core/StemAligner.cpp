#include "StemAligner.h"
#include <algorithm>

namespace mma {

void StemOffsetQueue::clear() noexcept
{
    head.store (0, std::memory_order_relaxed);
    tail.store (0, std::memory_order_relaxed);
}

bool StemOffsetQueue::push (const StemOffsetEvent& event) noexcept
{
    const auto h = head.load (std::memory_order_relaxed);
    const auto t = tail.load (std::memory_order_acquire);

    if (h - t >= kCapacity)
        return false;

    slots[h % kCapacity] = event;
    head.store (h + 1, std::memory_order_release);
    return true;
}

bool StemOffsetQueue::peek (StemOffsetEvent& event) const noexcept
{
    const auto t = tail.load (std::memory_order_relaxed);
    const auto h = head.load (std::memory_order_acquire);

    if (t == h)
        return false;

    event = slots[t % kCapacity];
    return true;
}

void StemOffsetQueue::pop() noexcept
{
    tail.store (tail.load (std::memory_order_relaxed) + 1, std::memory_order_release);
}

void StemAligner::prepare (int numChannels)
{
    lines.clear();

    for (int ch = 0; ch < std::max (0, numChannels); ++ch)
    {
        auto line = std::make_unique<Line>();

        // One more than the longest offset: a sample is put in before the
        // oldest one is taken out.
        line->buffer.assign (static_cast<size_t> (kMaxOffsetSamples) + 1, 0.0f);
        lines.push_back (std::move (line));
    }

    exact.store (true, std::memory_order_relaxed);
}

void StemAligner::setOffset (int channel, int samples, StemOffsetKind kind, int released) noexcept
{
    if (channel < 0 || channel >= static_cast<int> (lines.size()))
        return;

    auto& line = *lines[static_cast<size_t> (channel)];

    if (samples < 0 || samples > kMaxOffsetSamples)
        exact.store (false, std::memory_order_relaxed);

    const int wanted = std::clamp (samples, 0, kMaxOffsetSamples);
    const int current = line.offset.load (std::memory_order_relaxed);

    // Only the change straight after a provisional one can take it back.
    const auto undo = line.undo;
    line.undo = {};

    if (wanted == current)
        return;

    if (wanted > current)
    {
        // Held back further. First by no longer taking out silence still to
        // arrive (a shorter offset not yet fully applied), then by writing
        // silence now while the channel's audio waits behind it. Before its
        // first sample, that is the take's starting alignment rather than a
        // change to it.
        auto longer = static_cast<size_t> (wanted - current);

        const auto unskipped = std::min (longer, line.skipOwed);
        line.skipOwed -= unskipped;
        longer -= unskipped;

        // The device refused the block the last change was for: a driver
        // handed a backlog over in one piece, and the stream, having moved
        // nothing in the end, is back where it was. The silence that change
        // took out goes back where it was taken from -- behind whatever was
        // waiting ahead of it and has not gone out yet -- so the stem is as
        // if the backlog had never been mistaken for a growth, and neither
        // change is counted. Writing it at the head instead put the audio
        // still waiting from before the backlog's gap after the gap.
        if (kind == StemOffsetKind::refused && undo.valid && longer > 0)
        {
            const auto position = std::min (undo.ahead > undo.emitted ? undo.ahead - undo.emitted : size_t { 0 },
                                            line.size);
            const auto restored = insertSilence (line, position, std::min (longer, undo.silence));

            line.dropped.fetch_sub (restored, std::memory_order_relaxed);
            longer -= restored;
        }

        // The channel's own stream gave back room it held for a block its
        // device has left: it dropped that much quiet from the newest end of
        // what it had buffered, so its next sample is that much later in
        // the room than the newest one waiting here. The silence goes
        // exactly there -- behind everything waiting, which comes out on
        // time -- and is counted as silence written to hold the stem back.
        // At the head, as below, it landed `current` samples before the
        // quiet moment the stream skipped, inside whatever sound was there,
        // and put those samples that much late against every other stem.
        // Any more of the change is another device's growth: at the head.
        if (kind == StemOffsetKind::released && line.started && released > 0 && longer > 0)
        {
            const auto inserted = insertSilence (line, line.size,
                                                 std::min (longer, static_cast<size_t> (released)));

            line.silenceInserted.fetch_add (inserted, std::memory_order_relaxed);
            longer -= inserted;
        }

        (line.started ? line.silenceOwed : line.paddingOwed) += longer;
    }
    else
    {
        // Brought forward: first by not writing silence still owed. Then by
        // taking out the move's own silence -- a channel brought forward is
        // one whose stream has just moved later for its device's larger
        // block, and that move put exactly this much silence in its stream:
        // the run of it that has already arrived (the newest samples
        // waiting, while they are silent), and then as much of it as is
        // still arriving (process(), until a sample that is not silent
        // arrives). Audio is taken out only for what neither finds: the
        // newest samples waiting.
        auto shorter = static_cast<size_t> (current - wanted);

        const auto unwritten = std::min (shorter, line.silenceOwed);
        line.silenceOwed -= unwritten;
        shorter -= unwritten;

        const auto unpadded = std::min (shorter, line.paddingOwed);
        line.paddingOwed -= unpadded;
        shorter -= unpadded;

        const size_t capacity = line.buffer.size();
        size_t silentTail = 0;

        while (silentTail < shorter && silentTail < line.size
               && line.buffer[(line.head + line.size - 1 - silentTail) % capacity] == 0.0f)
            ++silentTail;

        if (silentTail > 0)
        {
            line.size -= silentTail;
            line.dropped.fetch_add (silentTail, std::memory_order_relaxed);
            shorter -= silentTail;
        }

        line.skipOwed += shorter;

        // For a block the device has not confirmed: remembered, so a refusal
        // can put this silence back (above). What is taken out as it arrives
        // is added to it in process().
        if (kind == StemOffsetKind::provisional)
            line.undo = { true, silentTail, line.size, 0 };
    }

    line.offset.store (wanted, std::memory_order_relaxed);
}

void StemAligner::dropNewest (Line& line, size_t count) noexcept
{
    const auto removed = std::min (count, line.size);
    line.size -= removed;

    if (removed > 0)
        line.dropped.fetch_add (removed, std::memory_order_relaxed);

    // Audio, not the move's silence: there is no putting that back.
    line.undo.valid = false;
}

size_t StemAligner::insertSilence (Line& line, size_t position, size_t count) noexcept
{
    // Never past the storage; the offset's own bound keeps it well inside.
    const size_t capacity = line.buffer.size();
    count = std::min (count, capacity - 1 - std::min (line.size, capacity - 1));
    position = std::min (position, line.size);

    if (count == 0)
        return 0;

    // The samples from `position` on (the newer ones) move along by `count`,
    // newest first so none is overwritten before it has moved, and silence
    // fills the space they leave. Bounded by the line's length, on the writer
    // thread, once for a refused block; at the newest end (room given back)
    // nothing moves.
    for (size_t i = line.size; i-- > position;)
        line.buffer[(line.head + i + count) % capacity] = line.buffer[(line.head + i) % capacity];

    for (size_t i = 0; i < count; ++i)
        line.buffer[(line.head + position + i) % capacity] = 0.0f;

    line.size += count;
    return count;
}

void StemAligner::process (int channel, float* samples, size_t frames) noexcept
{
    if (channel < 0 || channel >= static_cast<int> (lines.size()) || samples == nullptr || frames == 0)
        return;

    auto& line = *lines[static_cast<size_t> (channel)];

    if (! line.started)
    {
        line.started = true;
        line.startOffset.store (line.offset.load (std::memory_order_relaxed), std::memory_order_relaxed);
    }

    // Nothing held back and nothing owed: the channel passes straight
    // through, which is every channel of a rig whose devices agree.
    if (line.size == 0 && line.paddingOwed == 0 && line.silenceOwed == 0 && line.skipOwed == 0)
        return;

    const size_t capacity = line.buffer.size();
    uint64_t silenceWritten = 0;
    uint64_t silenceSkipped = 0;

    for (size_t f = 0; f < frames; ++f)
    {
        const float in = samples[f];

        if (line.skipOwed > 0)
        {
            if (in == 0.0f)
            {
                // The rest of the move's silence, arriving: not kept. The
                // channel comes forward by a sample; what goes out is the
                // oldest sample waiting.
                --line.skipOwed;
                ++silenceSkipped;
                samples[f] = line.buffer[line.head];
                line.head = (line.head + 1) % capacity;
                --line.size;

                if (line.undo.valid)
                {
                    ++line.undo.silence;
                    ++line.undo.emitted;
                }

                continue;
            }

            // Audio, and the move is not all accounted for: the rest comes
            // out of the newest samples waiting.
            dropNewest (line, line.skipOwed);
            line.skipOwed = 0;
        }

        line.buffer[(line.head + line.size) % capacity] = in;
        ++line.size;

        if (line.paddingOwed > 0)
        {
            samples[f] = 0.0f;
            --line.paddingOwed;
        }
        else if (line.silenceOwed > 0)
        {
            samples[f] = 0.0f;
            --line.silenceOwed;
            ++silenceWritten;
        }
        else
        {
            samples[f] = line.buffer[line.head];
            line.head = (line.head + 1) % capacity;
            --line.size;

            if (line.undo.valid)
                ++line.undo.emitted;
        }
    }

    if (silenceWritten > 0)
        line.silenceInserted.fetch_add (silenceWritten, std::memory_order_relaxed);

    if (silenceSkipped > 0)
        line.dropped.fetch_add (silenceSkipped, std::memory_order_relaxed);
}

int StemAligner::getOffset (int channel) const noexcept
{
    return channel >= 0 && channel < static_cast<int> (lines.size())
               ? lines[static_cast<size_t> (channel)]->offset.load (std::memory_order_relaxed) : 0;
}

int StemAligner::getStartOffset (int channel) const noexcept
{
    if (channel < 0 || channel >= static_cast<int> (lines.size()))
        return 0;

    const auto& line = *lines[static_cast<size_t> (channel)];
    const int start = line.startOffset.load (std::memory_order_relaxed);

    // Not started yet: what it will start with is the offset set for it now.
    return start >= 0 ? start : line.offset.load (std::memory_order_relaxed);
}

bool StemAligner::hasStarted (int channel) const noexcept
{
    return channel >= 0 && channel < static_cast<int> (lines.size())
           && lines[static_cast<size_t> (channel)]->startOffset.load (std::memory_order_relaxed) >= 0;
}

uint64_t StemAligner::getSilenceInserted (int channel) const noexcept
{
    return channel >= 0 && channel < static_cast<int> (lines.size())
               ? lines[static_cast<size_t> (channel)]->silenceInserted.load (std::memory_order_relaxed) : 0;
}

uint64_t StemAligner::getSamplesDropped (int channel) const noexcept
{
    return channel >= 0 && channel < static_cast<int> (lines.size())
               ? lines[static_cast<size_t> (channel)]->dropped.load (std::memory_order_relaxed) : 0;
}

} // namespace mma
