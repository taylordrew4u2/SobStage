#pragma once
#include <array>
#include <atomic>
#include <cstddef>

namespace mma {

/// Carries changes tagged with the take frame they apply from -- a stem's
/// alignment offset, a channel's weight in MIX.wav -- from the audio thread
/// to the writer thread, beside the audio itself. The audio thread queues a
/// change before the block it applies from goes into the ring, so a writer
/// that has read that block has already been handed it.
///
/// Single producer (the callback handing blocks to WritePipeline), single
/// consumer (the writer). Fixed storage, no locks and no allocation on
/// either side (§11).
template <typename Event, size_t Capacity>
class FrameEventQueue
{
public:
    static constexpr size_t kCapacity = Capacity;

    /// Not concurrent with push or pop: before the take's audio starts.
    void clear() noexcept
    {
        head.store (0, std::memory_order_relaxed);
        tail.store (0, std::memory_order_relaxed);
    }

    /// Producer. False when full; nothing is queued then.
    bool push (const Event& event) noexcept
    {
        const auto h = head.load (std::memory_order_relaxed);
        const auto t = tail.load (std::memory_order_acquire);

        if (h - t >= kCapacity)
            return false;

        slots[h % kCapacity] = event;
        head.store (h + 1, std::memory_order_release);
        return true;
    }

    /// Consumer: the oldest change, without taking it.
    bool peek (Event& event) const noexcept
    {
        const auto t = tail.load (std::memory_order_relaxed);
        const auto h = head.load (std::memory_order_acquire);

        if (t == h)
            return false;

        event = slots[t % kCapacity];
        return true;
    }

    /// Consumer: takes the oldest change. Only after a successful peek.
    void pop() noexcept
    {
        tail.store (tail.load (std::memory_order_relaxed) + 1, std::memory_order_release);
    }

private:
    std::array<Event, Capacity> slots {};
    std::atomic<size_t> head { 0 }; // next slot the producer writes
    std::atomic<size_t> tail { 0 }; // next slot the consumer reads
};

} // namespace mma
