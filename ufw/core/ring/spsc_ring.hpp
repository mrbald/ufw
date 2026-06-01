/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Fixed-size single-producer / single-consumer ring of trivially-copyable T.
 * Lock-free and wait-free on the fast path. The hard part — cursors, ordering,
 * back-pressure, cached cursors — lives in producer<Gate>/spsc_gate so SPMC
 * later is a gate swap, not a rewrite. This is the fixed-record special case of
 * the (later) variable-length byte_ring.
 */
#pragma once

#include "sequencer.hpp"

#include <ufw/core/mem/memory_region.hpp>

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ufw::core {

template <class T>
class spsc_ring
{
    static_assert(std::is_trivially_copyable_v<T>, "spsc_ring<T> requires a trivially-copyable T");

public:
    // capacity (slot count) is rounded up to a power of two; min 1.
    explicit spsc_ring(std::size_t min_slots):
        n_slots_{std::bit_ceil(min_slots < 1 ? std::size_t{1} : min_slots)},
        mask_{n_slots_ - 1},
        region_{{.bytes = n_slots_ * sizeof(T)}},
        slots_{reinterpret_cast<T*>(region_.data())},
        producer_{producer_pos_, spsc_gate{consumer_completed_}, n_slots_}
    {
        // The ring is pinned: producer_/spsc_gate hold pointers into this object,
        // and the cross-thread cursors are atomics — so it is neither copyable nor
        // movable (enforced implicitly by the std::atomic members).
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return n_slots_; }

    // --- producer side (one thread) ---

    // Reserve the next slot for in-place writing, or nullptr if full. Pair with commit().
    [[nodiscard]] T* try_claim() noexcept
    {
        auto const pos = producer_.claim(1);
        return pos ? slots_ + (*pos & mask_) : nullptr;
    }

    // Publish the claimed slot to the consumer.
    void commit() noexcept { producer_.publish(); }

    // Copy a value in; false if the ring is full (back-pressure).
    bool try_push(T const& value) noexcept
    {
        T* const target = try_claim();
        if (target == nullptr)
        {
            return false;
        }
        *target = value;
        commit();
        return true;
    }

    // --- consumer side (one thread) ---

    // Peek the next readable slot, or nullptr if empty. Pair with release().
    [[nodiscard]] T const* try_peek() noexcept
    {
        if (read_pos_ == cached_producer_)
        {
            cached_producer_ = producer_pos_.load(std::memory_order_acquire);
            if (read_pos_ == cached_producer_)
            {
                return nullptr;
            }
        }
        return slots_ + (read_pos_ & mask_);
    }

    // Mark the peeked slot consumed, freeing it for the producer.
    void release() noexcept
    {
        ++read_pos_;
        consumer_completed_.store(read_pos_, std::memory_order_release);
    }

    // Copy a value out; false if the ring is empty.
    bool try_pop(T& out) noexcept
    {
        T const* const source = try_peek();
        if (source == nullptr)
        {
            return false;
        }
        out = *source;
        release();
        return true;
    }

private:
    // Immutable after construction — read-only sharing, no false sharing.
    std::size_t   n_slots_;
    std::uint64_t mask_;
    memory_region region_;
    T*            slots_;

    // Cross-thread cursors, each isolated on its own cache line.
    alignas(cache_line) std::atomic<std::uint64_t> producer_pos_{0};       // producer -> consumer
    alignas(cache_line) std::atomic<std::uint64_t> consumer_completed_{0}; // consumer -> producer (gate)

    // Consumer-local working state (one consumer-only line).
    alignas(cache_line) std::uint64_t cached_producer_{0};
    std::uint64_t read_pos_{0};

    // Producer-local working state (claim_next_, cached_gate_) on its own line.
    alignas(cache_line) producer<spsc_gate> producer_;
};

} // namespace ufw::core
