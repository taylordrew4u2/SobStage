#include "RingBuffer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mma {

RingBuffer::RingBuffer (size_t capacitySamples)
    : buffer (std::max<size_t> (capacitySamples, 1))
{
}

void RingBuffer::reset (size_t capacitySamples)
{
    buffer.assign (std::max<size_t> (capacitySamples, 1), 0.0f);
    writeIndex.store (0, std::memory_order_relaxed);
    readIndex.store (0, std::memory_order_relaxed);
}

void RingBuffer::clear() noexcept
{
    // Consumer-side only, so this races with nothing: the producer moves
    // writeIndex and never reads readIndex for correctness of its own writes.
    readIndex.store (writeIndex.load (std::memory_order_acquire), std::memory_order_release);
}

size_t RingBuffer::discard (size_t numSamples) noexcept
{
    const size_t capacity = buffer.size();
    const size_t toDrop = std::min (numSamples, availableForRead());

    if (toDrop == 0)
        return 0;

    size_t r = readIndex.load (std::memory_order_relaxed) + toDrop;
    if (r >= capacity)
        r -= capacity;

    readIndex.store (r, std::memory_order_release);
    return toDrop;
}

float RingBuffer::peekPeak (size_t numSamples) const noexcept
{
    const size_t capacity = buffer.size();
    const size_t count = std::min (numSamples, availableForRead());
    size_t r = readIndex.load (std::memory_order_relaxed);
    float peak = 0.0f;

    for (size_t i = 0; i < count; ++i)
    {
        peak = std::max (peak, std::abs (buffer[r]));

        if (++r == capacity)
            r = 0;
    }

    return peak;
}

size_t RingBuffer::availableForRead() const noexcept
{
    // Both loads acquire: this is called from the consumer (which must see the
    // producer's samples before the index that publishes them) AND from the
    // producer via availableForWrite (which must see the consumer's release of
    // space). A relaxed load on the producer side was safe only by accident --
    // it under-reported free space rather than over-reporting it -- but it left
    // the pairing unspecified, which is not something to leave in an audio path.
    const size_t w = writeIndex.load (std::memory_order_acquire);
    const size_t r = readIndex.load (std::memory_order_acquire);
    return (w >= r) ? (w - r) : (buffer.size() - r + w);
}

size_t RingBuffer::availableForWrite() const noexcept
{
    // Reserve one slot to disambiguate full vs empty.
    return buffer.size() - 1 - availableForRead();
}

size_t RingBuffer::write (const float* data, size_t numSamples) noexcept
{
    const size_t capacity = buffer.size();
    const size_t toWrite = std::min (numSamples, availableForWrite());

    if (toWrite == 0)
        return 0;

    size_t w = writeIndex.load (std::memory_order_relaxed);

    // Two memcpys rather than a per-sample loop with a modulo in it. This runs
    // on the audio thread for every sample of every channel, and an integer
    // division per sample is real time the callback does not have to spend.
    const size_t firstChunk = std::min (toWrite, capacity - w);
    std::memcpy (buffer.data() + w, data, firstChunk * sizeof (float));

    if (toWrite > firstChunk)
        std::memcpy (buffer.data(), data + firstChunk, (toWrite - firstChunk) * sizeof (float));

    w += toWrite;
    if (w >= capacity)
        w -= capacity;

    writeIndex.store (w, std::memory_order_release);
    return toWrite;
}

size_t RingBuffer::read (float* data, size_t numSamples) noexcept
{
    const size_t capacity = buffer.size();
    const size_t toRead = std::min (numSamples, availableForRead());

    if (toRead == 0)
        return 0;

    size_t r = readIndex.load (std::memory_order_relaxed);

    const size_t firstChunk = std::min (toRead, capacity - r);
    std::memcpy (data, buffer.data() + r, firstChunk * sizeof (float));

    if (toRead > firstChunk)
        std::memcpy (data + firstChunk, buffer.data(), (toRead - firstChunk) * sizeof (float));

    r += toRead;
    if (r >= capacity)
        r -= capacity;

    readIndex.store (r, std::memory_order_release);
    return toRead;
}

double RingBuffer::fillFraction() const noexcept
{
    if (buffer.size() <= 1)
        return 0.0;
    return static_cast<double> (availableForRead()) / static_cast<double> (buffer.size() - 1);
}

size_t RingBuffer::minimumCapacitySamples (double sampleRate, int numChannels) noexcept
{
    const size_t thirtySeconds = static_cast<size_t> (sampleRate * 30.0 * std::max (1, numChannels));
    const size_t sixtyFourMbInFloats = (64ull * 1024ull * 1024ull) / sizeof (float);
    return std::max (thirtySeconds, sixtyFourMbInFloats);
}

} // namespace mma
