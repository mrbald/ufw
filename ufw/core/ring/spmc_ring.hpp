/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Fixed-size single-producer / multi-consumer ring of trivially-copyable T:
 * N consumers compete, each record delivered to exactly one (work-sharing,
 * the Disruptor WorkerPool model). The producer and the sequencer are IDENTICAL
 * to spsc_ring — the ONLY difference is the gate (min-of-N completions instead
 * of a single cursor). That is the whole point of the gate abstraction: SPSC and
 * SPMC share their hard part.
 *
 * Each consumer THREAD must use a unique index in [0, consumers).
 */
#pragma once

#include "sequencer.hpp"

#include <ufw/core/mem/memory_region.hpp>

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace ufw::core {

template <class T>
class spmc_ring
{
    static_assert(std::is_trivially_copyable_v<T>, "spmc_ring<T> requires a trivially-copyable T");

public:
    spmc_ring(std::size_t min_slots, std::size_t consumers):
        n_slots_{std::bit_ceil(min_slots < 1 ? std::size_t{1} : min_slots)},
        mask_{n_slots_ - 1},
        region_{{.bytes = n_slots_ * sizeof(T)}},
        slots_{reinterpret_cast<T*>(region_.data())},
        n_consumers_{consumers < 1 ? std::size_t{1} : consumers},
        completed_(n_consumers_),
        producer_{producer_pos_, spmc_gate{completed_.data(), n_consumers_}, n_slots_}
    {
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return n_slots_; }
    [[nodiscard]] std::size_t consumers() const noexcept { return n_consumers_; }

    // --- producer side (one thread) --- IDENTICAL to spsc_ring (the proof).
    [[nodiscard]] T* try_claim() noexcept
    {
        auto const pos = producer_.claim(1);
        return pos ? slots_ + (*pos & mask_) : nullptr;
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

    // --- consumer side (N threads; `index` unique per consumer thread) ---
    // Atomically claim the next published record and deliver it to exactly this
    // caller; false if none is currently available.
    bool try_consume(std::size_t index, T& out) noexcept
    {
        std::uint64_t pos = claim_cursor_.load(std::memory_order_relaxed);
        for (;;)
        {
            // Only claim positions the producer has published (acquire-load makes
            // the record's bytes visible before we read the slot below).
            if (pos >= producer_pos_.load(std::memory_order_acquire))
            {
                return false;
            }
            // Win exactly this position; the loser retries with the reloaded pos.
            if (claim_cursor_.compare_exchange_weak(pos, pos + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
            {
                break;
            }
        }
        out = slots_[pos & mask_];
        // Publish completion AFTER the read (release): the producer may now pass
        // this position in its min-gate and reuse the slot.
        completed_[index].value.store(pos + 1, std::memory_order_release);
        return true;
    }

private:
    std::size_t   n_slots_;
    std::uint64_t mask_;
    memory_region region_;
    T*            slots_;
    std::size_t   n_consumers_;
    std::vector<padded_sequence> completed_; // per-consumer completion (the gate's input)

    alignas(cache_line) std::atomic<std::uint64_t> producer_pos_{0}; // producer -> consumers
    alignas(cache_line) std::atomic<std::uint64_t> claim_cursor_{0}; // consumers CAS for work distribution

    alignas(cache_line) producer<spmc_gate> producer_;
};

} // namespace ufw::core
