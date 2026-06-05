/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * The shared, correctness-critical core of every ring: the producer's claim/
 * publish over absolute monotonic positions, and the "gate" through which it
 * sees the consumer side. SPSC uses a single-cursor gate; multicast swaps in a
 * min-of-N-cursors gate (min over the subscriber read positions) with ZERO change
 * to the producer — only the gate differs.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace ufw::core {

// False-sharing separation. We use a fixed per-arch value rather than
// std::hardware_destructive_interference_size to avoid GCC's -Winterference-size
// ABI warning (which -Werror would promote to an error) and libc++'s
// inconsistent support for the constant.
#if defined(__aarch64__) || defined(__arm64__) || defined(__APPLE__)
inline constexpr std::size_t cache_line = 128; // Apple Silicon / arm64 pair lines
#else
inline constexpr std::size_t cache_line = 64;  // x86-64
#endif

// The producer's only view of the consumer side: the completion floor — the
// smallest position still needed by any consumer. Positions below it are free to
// overwrite. The producer treats this as an opaque scalar; how it is computed
// (one cursor for SPSC, a min over the N subscribers for multicast) is the only
// thing that differs between ring flavours.
class spsc_gate
{
public:
    explicit spsc_gate(std::atomic<std::uint64_t> const& completed) noexcept:
        completed_{&completed} {}

    [[nodiscard]] std::uint64_t position() const noexcept
    {
        return completed_->load(std::memory_order_acquire);
    }

private:
    std::atomic<std::uint64_t> const* completed_;
};

// A completion sequence isolated on its own cache line, so competing consumers
// publishing their progress don't false-share with each other.
struct alignas(cache_line) padded_sequence
{
    std::atomic<std::uint64_t> value{0};
};

// The min over N subscriber cursors — the "everyone is at least here" floor: the
// multicast producer is gated by the SLOWEST subscriber. Each subscriber walks the
// whole stream through its OWN sequential cursor, so the min is a correct "all
// positions below are read by everyone" watermark, and the producer is identical
// to spsc_ring — it just reads this gate instead of the single-cursor spsc_gate.
//
// NB: a min-of-cursors gate is correct ONLY for sequential per-reader cursors.
// Work-sharing (competing consumers with sparse claims) needs per-slot turn
// sequencing instead — which is why that discipline is not built on this gate.
class multicast_gate
{
public:
    multicast_gate(padded_sequence const* completed, std::size_t count) noexcept:
        completed_{completed}, count_{count} {}

    [[nodiscard]] std::uint64_t position() const noexcept
    {
        std::uint64_t floor = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t i = 0; i < count_; ++i)
        {
            floor = std::min(floor, completed_[i].value.load(std::memory_order_acquire));
        }
        return floor;
    }

private:
    padded_sequence const* completed_;
    std::size_t count_;
};

// An O(1)-on-the-common-path alternative to multicast_gate: the SAME min-over-N
// floor, same ctor and role, but it avoids the full N-cursor scan when there is a
// PERSISTENT laggard. It caches the slowest cursor's index plus a "trip" watermark
// (the second-smallest cursor at the last scan). While the laggard stays at or
// below the trip it alone is the floor, so position() reads ONE cursor; only when
// the laggard overtakes the runner-up does it rescan all N and recompute the cache.
//
// Correct for the same reason multicast_gate is (sequential per-reader cursors),
// resting on cursor MONOTONICITY: at the last scan every non-laggard was >= trip,
// and cursors only move forward, so a laggard <= trip is provably still the global
// minimum. Wins big with one slow reader dragging the floor; degrades to a full
// scan per call when readers move in lock-step (trip == floor). The cache is
// producer-LOCAL (the single producer owns its gate), so it needs no
// synchronization; position() is non-const only because it mutates that cache.
class lazy_min_gate
{
public:
    lazy_min_gate(padded_sequence const* completed, std::size_t count) noexcept:
        completed_{completed}, count_{count}
    {
        rescan();
    }

    [[nodiscard]] std::uint64_t position() noexcept
    {
        std::uint64_t const cur = completed_[slow_].value.load(std::memory_order_acquire);
        if (cur <= trip_)
        {
            return cur;   // cached laggard is still the floor — one cursor read
        }
        return rescan();  // laggard overtook the runner-up — rescan and recache
    }

private:
    // Find the smallest cursor (the new laggard) and the second-smallest (the trip
    // watermark). O(N), runs only on a cache miss.
    std::uint64_t rescan() noexcept
    {
        std::uint64_t lo  = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t lo2 = std::numeric_limits<std::uint64_t>::max();
        std::size_t   lo_id = 0;
        for (std::size_t i = 0; i < count_; ++i)
        {
            std::uint64_t const v = completed_[i].value.load(std::memory_order_acquire);
            if (v < lo)
            {
                lo2 = lo;
                lo = v;
                lo_id = i;
            }
            else if (v < lo2)
            {
                lo2 = v;
            }
        }
        slow_ = lo_id;
        trip_ = lo2;
        return lo;
    }

    padded_sequence const* completed_;
    std::size_t   count_;
    std::size_t   slow_ = 0;  // index of the cached slowest cursor
    std::uint64_t trip_ = 0;  // floor stays valid while completed_[slow_] <= trip_
};

// Single-producer claim/publish over absolute monotonic positions. Reads free
// space through an abstract gate (the completion floor), caching it and only
// refreshing on apparent back-pressure (the standard cached-cursor optimization).
// `capacity` is in position units (slots for a typed ring, bytes for a byte ring).
template <class Gate>
class producer
{
public:
    producer(std::atomic<std::uint64_t>& cursor, Gate gate, std::uint64_t capacity) noexcept:
        cursor_{&cursor}, gate_{gate}, capacity_{capacity} {}

    // Reserve `n` positions; returns the start position, or nullopt if the claim
    // would overtake the gate (back-pressure). Never blocks — the wait policy is
    // the caller's.
    [[nodiscard]] std::optional<std::uint64_t> claim(std::uint64_t n) noexcept
    {
        std::uint64_t const end = claim_next_ + n;
        if (end - cached_gate_ > capacity_)        // would overrun unconsumed data?
        {
            cached_gate_ = gate_.position();        // the only gate read — refresh and retry
            if (end - cached_gate_ > capacity_)
            {
                return std::nullopt;                // still full: back-pressure
            }
        }
        std::uint64_t const pos = claim_next_;
        claim_next_ = end;
        return pos;
    }

    // Claim AS MANY as the gate allows, up to `n` (may be 0). Advances the claim
    // position by the granted count and returns it; the run starts at claimed().
    // The partial-friendly sibling of claim() — for batch producers that take
    // whatever space is free rather than all-or-nothing. Reads the gate at most once
    // (only when `n` does not obviously fit the cached floor), like claim().
    [[nodiscard]] std::uint64_t claim_upto(std::uint64_t n) noexcept
    {
        if (claim_next_ + n - cached_gate_ > capacity_)   // can't fit all n vs cached floor?
        {
            cached_gate_ = gate_.position();              // the only gate read — refresh
        }
        std::uint64_t const room = capacity_ - (claim_next_ - cached_gate_);
        std::uint64_t const grant = n < room ? n : room;
        claim_next_ += grant;
        return grant;
    }

    // Publish all claims so far (release): consumers may now read up to here.
    // const because it only reflects the producer's claim position to the cursor;
    // it does not change the producer's own state.
    void publish() const noexcept
    {
        cursor_->store(claim_next_, std::memory_order_release);
    }

    [[nodiscard]] std::uint64_t claimed() const noexcept { return claim_next_; }

private:
    std::atomic<std::uint64_t>* cursor_; // published position (producer -> consumers)
    Gate          gate_;
    std::uint64_t capacity_;
    std::uint64_t claim_next_ = 0;       // producer-local: next position to hand out
    std::uint64_t cached_gate_ = 0;      // producer-local: cached completion floor
};

} // namespace ufw::core
