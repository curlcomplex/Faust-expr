#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>

namespace curlop::transport {

// Lock-free audio-producer/message-consumer range handoff. The producer owns
// one slot until the consumer atomically retires it. A short sequence guard
// lets the non-realtime consumer wait for a publication that had already
// selected the retired slot, while the audio producer never waits or locks.
class ControlRangeHandoff
{
public:
    using InterleaveHook = void (*) (void*) noexcept;

    void publish(float minimum, float maximum, float last,
                 InterleaveHook afterSlotSelected = nullptr,
                 void* hookContext = nullptr) noexcept
    {
        producerSequence_.fetch_add(1u, std::memory_order_acq_rel);
        const auto slotIndex = activeSlot_.load(std::memory_order_acquire);
        if (afterSlotSelected != nullptr)
            afterSlotSelected(hookContext);
        auto& slot = slots_[slotIndex];
        if (! slot.seen) {
            slot.minimum = minimum;
            slot.maximum = maximum;
            slot.seen = true;
        } else {
            slot.minimum = std::min(slot.minimum, minimum);
            slot.maximum = std::max(slot.maximum, maximum);
        }
        slot.last = last;
        producerSequence_.fetch_add(1u, std::memory_order_release);
    }

    bool exchange(float& minimum, float& maximum, float& last,
                  InterleaveHook afterSlotRetired = nullptr,
                  void* hookContext = nullptr) noexcept
    {
        const auto current = activeSlot_.load(std::memory_order_relaxed);
        const auto retired = activeSlot_.exchange(
            1u - current, std::memory_order_acq_rel);
        if (afterSlotRetired != nullptr)
            afterSlotRetired(hookContext);

        // A producer that selected the retired slot must finish before it is
        // read. A producer beginning after the swap selects the other slot.
        for (;;) {
            const auto before = producerSequence_.load(
                std::memory_order_acquire);
            if ((before & 1u) != 0u) {
                std::this_thread::yield();
                continue;
            }
            const auto after = producerSequence_.load(
                std::memory_order_acquire);
            if (before == after)
                break;
        }

        auto& slot = slots_[retired];
        if (! slot.seen) {
            minimum = 0.0f;
            maximum = 0.0f;
            last = 0.0f;
            return false;
        }
        minimum = slot.minimum;
        maximum = slot.maximum;
        last = slot.last;
        slot = {};
        return true;
    }

private:
    struct Slot {
        float minimum = 0.0f;
        float maximum = 0.0f;
        float last = 0.0f;
        bool seen = false;
    };

    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "prepared telemetry requires lock-free 32-bit atomics");
    Slot slots_[2];
    std::atomic<std::uint32_t> activeSlot_ { 0u };
    std::atomic<std::uint32_t> producerSequence_ { 0u };
};

} // namespace curlop::transport
