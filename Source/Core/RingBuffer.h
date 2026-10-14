#pragma once
#include <atomic>
#include <vector>
#include <cstddef>

namespace mma {

/// Lock-free single-producer/single-consumer ring buffer of float samples,
/// used to carry audio from the real-time callback (producer) to the writer
/// thread (consumer). No locks, no allocation after construction.
/// §6.3: sized for 30s at the current channel count/rate, minimum 64MB.
class RingBuffer
{
public:
    explicit RingBuffer (size_t capacitySamples);

    /// Resize (reallocates -- call only when NOT concurrently in use by the
    /// audio thread, e.g. before a recording starts).
    void reset (size_t capacitySamples);

    /// Producer (audio thread): write as many samples as fit; returns how many
    /// were actually written. Never blocks, never allocates.
    size_t write (const float* data, size_t numSamples) noexcept;

    /// Consumer (writer thread): read up to numSamples; returns how many were read.
    size_t read (float* data, size_t numSamples) noexcept;

    /// Consumer side: discard everything currently readable. Real-time safe --
    /// it moves the read index, so it neither allocates nor blocks, and only
    /// the consumer ever moves that index.
    ///
    /// This is not reset(): reset reallocates and may only be called while the
    /// audio thread is out of the picture. This is for the consumer deciding
    /// that what is buffered is no longer wanted, which is what a capture path
    /// resuming after a gap has to say about the audio from before it.
    void clear() noexcept;

    /// Consumer side: drop up to numSamples of what is readable, oldest first,
    /// without copying them out; returns how many were dropped. Real-time
    /// safe for the same reason clear() is. This is for a consumer skipping
    /// audio it has decided not to play -- a span that arrived late, after
    /// silence had already stood in for it.
    size_t discard (size_t numSamples) noexcept;

    /// Consumer side: the largest magnitude among the next numSamples
    /// readable samples (or all of them, if fewer), without consuming any.
    /// Real-time safe: it reads what the producer has already published and
    /// moves nothing. For a consumer deciding whether a span it may skip is
    /// quiet.
    float peekPeak (size_t numSamples) const noexcept;

    /// Number of samples currently readable.
    size_t availableForRead() const noexcept;

    /// Free space, in samples, currently writable.
    size_t availableForWrite() const noexcept;

    size_t capacity() const noexcept { return buffer.size(); }

    /// Fill fraction 0..1, used to drive the drift-compensation PI loop and the
    /// §6.5 50%/90% overrun warnings.
    double fillFraction() const noexcept;

    /// Minimum recommended capacity per §6.3: 30 seconds at the given rate/channel
    /// count, floored at 64MB worth of float samples.
    static size_t minimumCapacitySamples (double sampleRate, int numChannels) noexcept;

private:
    std::vector<float> buffer;
    std::atomic<size_t> writeIndex { 0 };
    std::atomic<size_t> readIndex { 0 };
};

} // namespace mma
