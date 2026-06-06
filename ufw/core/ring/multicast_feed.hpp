/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 *
 * Fixed-size single-producer / multi-subscriber LOSSY MULTICAST buffer of
 * trivially-copyable T — the Aeron broadcast-buffer discipline, and the
 * complementary half of multicast_channel. Where multicast_channel is loss-free and
 * therefore GATED by the slowest subscriber, multicast_feed's producer NEVER
 * waits: it overwrites the oldest slot and runs at ~SPSC speed no matter how many
 * subscribers exist or how far behind they are. There is no subscriber registry
 * and no gate. A subscriber that falls behind is DETECTED (the producer lapped it)
 * and told how many records it lost; what happens next — skip, die, or later
 * request retransmission / a snapshot — is the caller's policy, a separate concern
 * the buffer knows nothing about.
 *
 * Per-slot seqlock, co-located with the payload. Each slot is { stamp, value } so a
 * reader's stamp/value/stamp seqlock and the producer's stamp/value/stamp write each
 * touch a SINGLE cache line (for small T) instead of bouncing two arrays. The stamp
 * is (position << 1) | writing-bit; 0 means "never written". The single producer
 * marks the slot writing (odd), overwrites the payload, then publishes done (even).
 * A feed_reader at its expected position E reads slot (E & mask):
 *   stamp == 0 or (>>1) < E -> empty   (slot still holds an older lap; E not produced)
 *   (>>1) == E, writing      -> empty   (E is being written right this moment)
 *   (>>1) == E, done         -> read the payload, re-check the stamp is unchanged -> ok
 *   (>>1)  > E               -> lapped: the producer overwrote E; resync forward to
 *                              the OLDEST position still in the buffer (salvage what
 *                              is left) and report how many were skipped.
 * Positions are 1-based so the all-zero "never written" stamp is unambiguous; the
 * published producer cursor (next position) lets a lapped reader land on the oldest
 * still-readable record instead of coarsely skipping a whole lap.
 *
 * NOTE: the payload load/store is the classic seqlock benign race (a torn read is
 * detected and discarded by the stamp re-check). It is correct on real hardware and
 * clean under ASan; ThreadSanitizer will flag it — to be made atomic_ref-clean when
 * the TSan job is wired.
 */
#pragma once

#include <ufw/core/mem/memory_region.hpp>

#include "sequencer.hpp" // cache_line

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace ufw::core {

enum class read_status : std::uint8_t { ok, empty, lapped };

namespace detail {
// One slot = its seqlock stamp co-located with its payload, so the seqlock's
// stamp/value/stamp accesses land on a single cache line for small T.
template <class T>
struct stamped_slot
{
    std::atomic<std::uint64_t> seq; // (position << 1) | writing-bit; 0 = never written
    T value;
};
} // namespace detail

// Independent, registry-free reader over a multicast_feed. Detects when the
// producer has lapped it and resyncs forward; move-only (copying would alias a
// position with no benefit).
template <class T>
class feed_reader
{
public:
    feed_reader(detail::stamped_slot<T> const* slots,
                  std::atomic<std::uint64_t> const* produce_pos,
                  std::uint64_t mask, std::uint64_t start_pos) noexcept:
        slots_{slots}, produce_pos_{produce_pos}, mask_{mask}, read_pos_{start_pos}
    {
    }

    ~feed_reader() = default;
    feed_reader(feed_reader&&) noexcept = default;
    feed_reader& operator=(feed_reader&&) noexcept = default;
    feed_reader(feed_reader const&) = delete;
    feed_reader& operator=(feed_reader const&) = delete;

    // Try to read the next record. ok: `out` is set and the position advanced by 1.
    // empty: nothing new (or being written). lapped: the producer overwrote records
    // before we reached them — `out` is untouched, the reader has resynced to the
    // oldest still-readable position, and `skipped` is how many records were lost.
    [[nodiscard]] read_status try_read(T& out, std::uint64_t& skipped) noexcept
    {
        detail::stamped_slot<T> const& s = slots_[read_pos_ & mask_];
        std::uint64_t const s0 = s.seq.load(std::memory_order_acquire);
        if (s0 == 0)
        {
            return read_status::empty;             // slot never written
        }
        std::uint64_t const pos0 = s0 >> 1;
        if (pos0 < read_pos_)
        {
            return read_status::empty;             // older lap still here -> E not yet produced
        }
        if (pos0 > read_pos_)
        {
            return resync(skipped, pos0);          // producer lapped us
        }
        if ((s0 & 1U) != 0U)
        {
            return read_status::empty;             // E is being written right now
        }
        // stable at E: read the payload, then re-check the stamp did not move.
        out = s.value;
        std::atomic_thread_fence(std::memory_order_acquire);
        std::uint64_t const s1 = s.seq.load(std::memory_order_relaxed);
        if (s1 != s0)
        {
            return resync(skipped, s1 >> 1);       // overwritten mid-read -> we were lapped
        }
        ++read_pos_;
        return read_status::ok;
    }

    [[nodiscard]] std::uint64_t position() const noexcept { return read_pos_; }

private:
    // We were lapped at read_pos_. Jump to the oldest position still in the buffer
    // (everything from there is salvageable) and report how many were lost. `floor`
    // is a position known to be > read_pos_ (a stamp the producer has already moved
    // past), used as a fallback if the published cursor reads stale.
    [[nodiscard]] read_status resync(std::uint64_t& skipped, std::uint64_t floor) noexcept
    {
        std::uint64_t const next = produce_pos_->load(std::memory_order_acquire);
        std::uint64_t const cap = mask_ + 1;
        std::uint64_t const oldest = next > cap ? next - cap : 1;
        std::uint64_t const target = oldest > read_pos_ ? oldest : floor;
        skipped = target - read_pos_;
        read_pos_ = target;
        return read_status::lapped;
    }

    detail::stamped_slot<T> const*    slots_;
    std::atomic<std::uint64_t> const* produce_pos_;
    std::uint64_t                     mask_;
    std::uint64_t                     read_pos_;
};

template <class T>
class multicast_feed
{
    static_assert(std::is_trivially_copyable_v<T>, "multicast_feed requires a trivially-copyable T");
    using slot = detail::stamped_slot<T>;

public:
    explicit multicast_feed(std::size_t min_slots):
        n_slots_{std::bit_ceil(min_slots < 1 ? std::size_t{1} : min_slots)},
        region_{{.bytes = n_slots_ * sizeof(slot)}},
        slots_{reinterpret_cast<slot*>(region_.data())}
    {
        for (std::size_t i = 0; i < n_slots_; ++i)
        {
            ::new (static_cast<void*>(slots_ + i)) slot{}; // start lifetimes; seq = 0, value = 0
        }
    }

    ~multicast_feed()
    {
        for (std::size_t i = 0; i < n_slots_; ++i)
        {
            (slots_ + i)->~slot();
        }
    }

    multicast_feed(multicast_feed const&) = delete;
    multicast_feed& operator=(multicast_feed const&) = delete;
    multicast_feed(multicast_feed&&) = delete;            // slots_ points into region_
    multicast_feed& operator=(multicast_feed&&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept { return n_slots_; }

    // Producer side (one thread). Never blocks; overwrites the oldest slot.
    void push(T const& value) noexcept
    {
        std::uint64_t const pos = produce_pos_.load(std::memory_order_relaxed); // producer-local
        slot& s = slots_[pos & mask()];
        s.seq.store((pos << 1) | 1U, std::memory_order_relaxed);        // writing (odd)
        std::atomic_thread_fence(std::memory_order_release);
        s.value = value;                                                // overwrite payload
        s.seq.store(pos << 1, std::memory_order_release);              // done (even)
        produce_pos_.store(pos + 1, std::memory_order_release);        // publish the live edge
    }

    // Hand out an independent reader, starting at the first position (1). If the
    // producer has already lapped past it, the first read reports a gap and resyncs
    // to the oldest still-readable record. (1-based: position 0 is the sentinel.)
    [[nodiscard]] feed_reader<T> subscribe() const noexcept
    {
        return feed_reader<T>{slots_, &produce_pos_, mask(), 1};
    }

private:
    [[nodiscard]] std::uint64_t mask() const noexcept { return n_slots_ - 1; }

    std::size_t   n_slots_;
    memory_region region_;
    slot*         slots_;
    alignas(cache_line) std::atomic<std::uint64_t> produce_pos_{1}; // next position to write; published
};

} // namespace ufw::core
