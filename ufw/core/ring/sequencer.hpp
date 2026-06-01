/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * The shared, correctness-critical core of every ring: the producer's claim/
 * publish over absolute monotonic positions, and the "gate" through which it
 * sees the consumer side. SPSC uses a single-cursor gate; SPMC later swaps in a
 * min-of-N-completions gate with ZERO change to the producer (the verified
 * Disruptor WorkerPool factoring).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
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
// (one cursor for SPSC, a min over N for SPMC) is the only thing that differs
// between ring flavours.
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

    // Publish all claims so far (release): consumers may now read up to here.
    void publish() noexcept
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
