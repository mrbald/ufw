/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 *
 * Fixed-size multi-producer / single-consumer ring of trivially-copyable T — the
 * classic Vyukov bounded queue (per-slot sequence stamps + a CAS-claimed enqueue
 * cursor), used MPSC. This is the "one inbox per worker" dispatch flavour's
 * substrate, the alternative to the N×N SPSC matrix: producers CONTEND on one
 * cursor (a CAS each) and an unpublished claim head-of-line-blocks the consumer,
 * in exchange for the consumer draining ONE ring regardless of fan-in and O(N)
 * instead of O(N²) memory. Which trade wins is an empirical question — see the
 * dispatch benchmarks (fan-in is where the two genuinely differ).
 *
 * Deliberately NOT built on producer<Gate>/slot_storage: those are single-producer
 * by construction (monotonic claim, no per-slot state); the Vyukov design needs a
 * per-slot seq lane. Per-producer FIFO holds (each push completes its claim before
 * returning); cross-producer order is claim order.
 */
#pragma once

#include "sequencer.hpp" // cache_line

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace ufw::core {

template <class T>
class mpsc_ring
{
    static_assert(std::is_trivially_copyable_v<T>, "ring slots move by memcpy");

    struct slot
    {
        std::atomic<std::uint64_t> seq;
        T value;
    };

public:
    explicit mpsc_ring(std::size_t min_slots):
        mask_{std::bit_ceil(min_slots < 2 ? 2 : min_slots) - 1},
        slots_(mask_ + 1) // in-place construction; never reallocated (slot is immovable)
    {
        for (std::uint64_t i = 0; i <= mask_; ++i)
        {
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

    // --- producer side (ANY thread) ---
    // False when full (the caller owns the back-pressure policy, as with spsc_ring).
    [[nodiscard]] bool try_push(T const& v) noexcept
    {
        std::uint64_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;)
        {
            slot& s = slots_[pos & mask_];
            std::uint64_t const seq = s.seq.load(std::memory_order_acquire);
            auto const dif = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos);
            if (dif == 0) // the slot is free at this position: try to claim it
            {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    s.value = v;
                    s.seq.store(pos + 1, std::memory_order_release); // publish
                    return true;
                }
                // CAS failure reloaded pos: retry against the new position
            }
            else if (dif < 0)
            {
                return false; // the slot is a full lap behind: ring full
            }
            else
            {
                pos = enqueue_pos_.load(std::memory_order_relaxed); // lost the race: refresh
            }
        }
    }

    // --- consumer side (ONE thread) ---
    // True when the next record is published (a cheap emptiness probe so an idle
    // poll turn costs one acquire load, no clock read, no callback setup).
    [[nodiscard]] bool ready() const noexcept
    {
        return slots_[dequeue_pos_ & mask_].seq.load(std::memory_order_acquire)
               == dequeue_pos_ + 1;
    }

    // Run fn over up to max_n published records, in order; returns the count.
    // Stops at the first unpublished slot (a claimed-but-unwritten producer slot
    // head-of-line blocks — inherent to the design, see the file header). fn is
    // taken by value (multi-invoked; ref-capturing lambdas copy for free).
    template <class F>
    std::size_t drain(F fn, std::size_t max_n) noexcept
    {
        std::size_t n = 0;
        while (n < max_n)
        {
            slot& s = slots_[dequeue_pos_ & mask_];
            if (s.seq.load(std::memory_order_acquire) != dequeue_pos_ + 1)
            {
                break;
            }
            fn(std::as_const(s.value));
            s.seq.store(dequeue_pos_ + mask_ + 1, std::memory_order_release); // recycle
            ++dequeue_pos_;
            ++n;
        }
        return n;
    }

private:
    std::uint64_t const mask_;
    std::vector<slot> slots_;

    alignas(cache_line) std::atomic<std::uint64_t> enqueue_pos_{0}; // producers contend here
    alignas(cache_line) std::uint64_t dequeue_pos_{0};              // consumer-local
};

} // namespace ufw::core
