/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Fixed-size single-producer / single-consumer ring of trivially-copyable T.
 * Lock-free and wait-free on the fast path. Cursors/back-pressure/ordering live
 * in producer<Gate>/spsc_gate (sequencer.hpp); the slot array in slot_storage<T>
 * (slots.hpp). The consumer is kept INLINE here (direct members, not the detached
 * reader<T>): for the SPSC hot path the pointer indirection of a detached reader
 * costs ~2x throughput, and the ring *is* its single consumer. broadcast_ring,
 * whose subscribers are genuinely separate objects, uses reader<T> instead.
 */
#pragma once

#include "sequencer.hpp"
#include "slots.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ufw::core {

template <class T>
class spsc_ring
{
public:
    explicit spsc_ring(std::size_t min_slots):
        storage_{min_slots},
        producer_{producer_pos_, spsc_gate{consumer_completed_}, storage_.capacity()}
    {
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.capacity(); }

    // --- producer side (one thread) ---
    [[nodiscard]] T* try_claim() noexcept
    {
        auto const pos = producer_.claim(1);
        return pos ? storage_.slot(*pos) : nullptr;
    }
    // Batch producer: claim a contiguous run of up to `n` slots — clamped to the
    // ring end so the span never straddles the wrap, and to free space. Fill the
    // span, then commit() ONCE to publish the whole batch, amortizing the cursor
    // release store over the run. May be shorter than `n`, or empty under back-
    // pressure; the caller loops to claim the tail (which starts at the wrap).
    [[nodiscard]] std::span<T> try_claim_batch(std::uint64_t n) noexcept
    {
        std::uint64_t const pos = producer_.claimed();
        std::uint64_t const to_wrap = storage_.capacity() - (pos & storage_.mask());
        std::uint64_t const granted = producer_.claim_upto(n < to_wrap ? n : to_wrap);
        return {storage_.slot(pos), granted};
    }
    void commit() noexcept { producer_.publish(); }
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

    // --- consumer side (one thread) --- inline; mirrors reader<T> but on direct
    // members (the SPSC fast path can't afford a detached reader's indirection).
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
        return storage_.slot(read_pos_);
    }
    // Batch consumer: peek a contiguous run of all currently-available records
    // [read_pos_, producer), clamped to the ring end. Process them, then release(k)
    // ONCE to publish progress, amortizing the cursor release store over the run.
    [[nodiscard]] std::span<T const> peek_batch() noexcept
    {
        if (read_pos_ == cached_producer_)
        {
            cached_producer_ = producer_pos_.load(std::memory_order_acquire);
            if (read_pos_ == cached_producer_)
            {
                return {};
            }
        }
        std::uint64_t const avail = cached_producer_ - read_pos_;
        std::uint64_t const to_wrap = storage_.capacity() - (read_pos_ & storage_.mask());
        return {storage_.slot(read_pos_), avail < to_wrap ? avail : to_wrap};
    }
    void release(std::uint64_t n = 1) noexcept
    {
        read_pos_ += n;
        consumer_completed_.store(read_pos_, std::memory_order_release);
    }
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
    slot_storage<T> storage_;

    alignas(cache_line) std::atomic<std::uint64_t> producer_pos_{0};       // producer -> consumer
    alignas(cache_line) std::atomic<std::uint64_t> consumer_completed_{0}; // consumer -> producer (gate)

    alignas(cache_line) std::uint64_t cached_producer_{0}; // consumer-local
    std::uint64_t read_pos_{0};

    alignas(cache_line) producer<spsc_gate> producer_;     // producer-local
};

} // namespace ufw::core
