/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Fixed-size single-producer / multi-subscriber BROADCAST ring of trivially-
 * copyable T: every subscriber sees EVERY record, each at its own pace (pub/sub
 * fan-out). This is the sibling of spmc_ring — same single-producer, but where
 * spmc_ring delivers each record to exactly ONE consumer (work-sharing), this
 * delivers each record to ALL of them.
 *
 * The whole point: the producer and the sequencer are IDENTICAL to spsc_ring /
 * spmc_ring. The gate is the same spmc_gate (min over N per-subscriber cursors);
 * here those cursors are READ positions rather than work-completion positions.
 * A subscriber is essentially an spsc_ring consumer, replicated N times, with the
 * producer gated by the SLOWEST subscriber (loss-free: a slot is not overwritten
 * until every subscriber has read it).
 *
 * Contract: all `subscribers` cursors are live from construction (the gate counts
 * them at position 0), so every subscriber sees the stream from position 0. ALL
 * of them must be subscribed and actively drained, or the producer stalls at
 * `capacity` (loss-free back-pressure on the slowest) — it never corrupts or
 * drops. Dynamic late-join (start mid-stream) is intentionally out of scope here.
 */
#pragma once

#include "sequencer.hpp"

#include <ufw/core/mem/memory_region.hpp>

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace ufw::core {

template <class T>
class broadcast_ring
{
    static_assert(std::is_trivially_copyable_v<T>, "broadcast_ring<T> requires a trivially-copyable T");

public:
    broadcast_ring(std::size_t min_slots, std::size_t subscribers):
        n_slots_{std::bit_ceil(min_slots < 1 ? std::size_t{1} : min_slots)},
        mask_{n_slots_ - 1},
        region_{{.bytes = n_slots_ * sizeof(T)}},
        slots_{reinterpret_cast<T*>(region_.data())},
        n_subs_{subscribers < 1 ? std::size_t{1} : subscribers},
        read_(n_subs_),
        producer_{producer_pos_, spmc_gate{read_.data(), n_subs_}, n_slots_}
    {
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return n_slots_; }
    [[nodiscard]] std::size_t subscribers() const noexcept { return n_subs_; }

    // --- producer side (one thread) --- IDENTICAL to spsc_ring / spmc_ring.
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

    // A single subscriber's independent read cursor over the broadcast stream.
    // Like an spsc_ring consumer: peek/release the next record, never skipping.
    // Move-only — copying would alias a cursor and corrupt the gate.
    class subscription
    {
    public:
        ~subscription() = default;
        subscription(subscription&&) noexcept = default;
        subscription& operator=(subscription&&) noexcept = default;
        subscription(subscription const&) = delete;
        subscription& operator=(subscription const&) = delete;

        // Next readable record for this subscriber, or nullptr if caught up.
        [[nodiscard]] T const* try_peek() noexcept
        {
            if (read_pos_ == cached_producer_)
            {
                cached_producer_ = ring_->producer_pos_.load(std::memory_order_acquire);
                if (read_pos_ == cached_producer_)
                {
                    return nullptr;
                }
            }
            return ring_->slots_ + (read_pos_ & ring_->mask_);
        }

        // Advance past the peeked record; publish this subscriber's progress so
        // the producer's min-gate may eventually reuse the slot (release: orders
        // this subscriber's read of the slot before any later overwrite).
        void release() noexcept
        {
            ++read_pos_;
            ring_->read_[index_].value.store(read_pos_, std::memory_order_release);
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
        friend class broadcast_ring;
        subscription(broadcast_ring& ring, std::size_t index) noexcept:
            ring_{&ring}, index_{index} {}

        broadcast_ring* ring_;
        std::size_t   index_;
        std::uint64_t read_pos_ = 0;
        std::uint64_t cached_producer_ = 0;
    };

    // Hand out the next of `subscribers` read cursors (thread-safe). Throws once
    // exhausted. All of them must be drained or the producer stalls.
    [[nodiscard]] subscription subscribe()
    {
        std::size_t const index = next_sub_.fetch_add(1, std::memory_order_relaxed);
        if (index >= n_subs_)
        {
            throw std::out_of_range("broadcast_ring: more subscribers than reserved");
        }
        return subscription{*this, index};
    }

private:
    std::size_t   n_slots_;
    std::uint64_t mask_;
    memory_region region_;
    T*            slots_;
    std::size_t   n_subs_;
    std::vector<padded_sequence> read_; // per-subscriber read cursor (the gate's input)

    alignas(cache_line) std::atomic<std::uint64_t> producer_pos_{0}; // producer -> subscribers
    alignas(cache_line) std::atomic<std::size_t>   next_sub_{0};     // subscribe() hand-out counter

    alignas(cache_line) producer<spmc_gate> producer_;
};

} // namespace ufw::core
