/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Fixed-size single-producer / multi-subscriber BROADCAST ring of trivially-
 * copyable T: every subscriber sees EVERY record, each at its own pace (pub/sub
 * fan-out). Sibling of spmc_ring — same single producer, but each record goes to
 * ALL subscribers instead of one.
 *
 * The whole point: the producer and the sequencer are IDENTICAL to spsc_ring /
 * spmc_ring, and it reuses spmc_gate (min-of-N) verbatim — here the N cursors are
 * subscriber READ positions. A subscriber is just a reader<T> (slots.hpp), the
 * same independent reader the SPSC consumer uses. The producer is gated by the
 * SLOWEST subscriber (loss-free: a slot is not overwritten until every subscriber
 * has read it).
 *
 * Contract: all `subscribers` cursors are live from construction (the gate counts
 * them from position 0), so every subscriber sees the stream from position 0. ALL
 * of them must be subscribed and drained, or the producer stalls at `capacity`
 * (loss-free back-pressure on the slowest) — it never corrupts or drops. Dynamic
 * late-join (start mid-stream) is intentionally out of scope here.
 */
#pragma once

#include "sequencer.hpp"
#include "slots.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace ufw::core {

template <class T>
class broadcast_ring
{
public:
    broadcast_ring(std::size_t min_slots, std::size_t subscribers):
        storage_{min_slots},
        n_subs_{subscribers < 1 ? std::size_t{1} : subscribers},
        read_(n_subs_),
        producer_{producer_pos_, spmc_gate{read_.data(), n_subs_}, storage_.capacity()}
    {
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.capacity(); }
    [[nodiscard]] std::size_t subscribers() const noexcept { return n_subs_; }

    // --- producer side (one thread) --- IDENTICAL to spsc_ring / spmc_ring.
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

    // Hand out the next of `subscribers` independent read cursors (thread-safe).
    // Each is a reader<T> that sees the whole stream in order. Throws once
    // exhausted; all of them must be drained or the producer stalls.
    [[nodiscard]] reader<T> subscribe()
    {
        std::size_t const index = next_sub_.fetch_add(1, std::memory_order_relaxed);
        if (index >= n_subs_)
        {
            throw std::out_of_range("broadcast_ring: more subscribers than reserved");
        }
        return reader<T>{storage_, producer_pos_, read_[index].value};
    }

private:
    slot_storage<T> storage_;
    std::size_t     n_subs_;
    std::vector<padded_sequence> read_; // per-subscriber read cursor (the gate's input)

    alignas(cache_line) std::atomic<std::uint64_t> producer_pos_{0}; // producer -> subscribers
    alignas(cache_line) std::atomic<std::size_t>   next_sub_{0};     // subscribe() hand-out counter

    alignas(cache_line) producer<spmc_gate> producer_;
};

} // namespace ufw::core
