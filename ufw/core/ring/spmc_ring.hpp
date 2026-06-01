/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Fixed-size single-producer / multi-consumer ring of trivially-copyable T:
 * N consumers compete, each record delivered to exactly one (work-sharing, the
 * Disruptor WorkerPool model). The producer and the sequencer are IDENTICAL to
 * spsc_ring / broadcast_ring — only the gate (min-of-N completions) and the
 * CAS-claim consumer are particular to work-sharing. Storage is slot_storage<T>.
 *
 * Each consumer THREAD must use a unique index in [0, consumers).
 */
#pragma once

#include "sequencer.hpp"
#include "slots.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ufw::core {

template <class T>
class spmc_ring
{
public:
    spmc_ring(std::size_t min_slots, std::size_t consumers):
        storage_{min_slots},
        n_consumers_{consumers < 1 ? std::size_t{1} : consumers},
        completed_(n_consumers_),
        producer_{producer_pos_, spmc_gate{completed_.data(), n_consumers_}, storage_.capacity()}
    {
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.capacity(); }
    [[nodiscard]] std::size_t consumers() const noexcept { return n_consumers_; }

    // --- producer side (one thread) --- IDENTICAL to spsc_ring / broadcast_ring.
    [[nodiscard]] T* try_claim() noexcept
    {
        auto const pos = producer_.claim(1);
        return pos ? storage_.slot(*pos) : nullptr;
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
        out = *storage_.slot(pos);
        // Publish completion AFTER the read (release): the producer may now pass
        // this position in its min-gate and reuse the slot.
        completed_[index].value.store(pos + 1, std::memory_order_release);
        return true;
    }

private:
    slot_storage<T> storage_;
    std::size_t     n_consumers_;
    std::vector<padded_sequence> completed_; // per-consumer completion (the gate's input)

    alignas(cache_line) std::atomic<std::uint64_t> producer_pos_{0}; // producer -> consumers
    alignas(cache_line) std::atomic<std::uint64_t> claim_cursor_{0}; // consumers CAS for work distribution

    alignas(cache_line) producer<spmc_gate> producer_;
};

} // namespace ufw::core
