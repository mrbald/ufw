/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Fixed-size single-producer / multi-subscriber LOSSY MULTICAST buffer of
 * trivially-copyable T — the Aeron broadcast-buffer discipline, and the
 * complementary half of multicast_ring. Where multicast_ring is loss-free and
 * therefore GATED by the slowest subscriber, multicast_buffer's producer NEVER
 * waits: it overwrites the oldest slot and runs at ~SPSC speed no matter how many
 * subscribers exist or how far behind they are. There is no subscriber registry
 * and no gate. A subscriber that falls behind is DETECTED (the producer lapped it)
 * and told how many records it lost; what happens next — skip, die, or later
 * request retransmission / a snapshot — is the caller's policy, a separate concern
 * the buffer knows nothing about.
 *
 * Per-slot seqlock. Each slot carries a stamp = (position << 1) | writing-bit;
 * 0 means "never written". The single producer marks the slot writing (odd),
 * overwrites the payload, then publishes done (even). A buffer_reader at its
 * expected position E reads stamp s of slot (E & mask):
 *   s == 0 or (s>>1) < E   -> empty   (slot still holds an older lap; E not produced)
 *   (s>>1) == E, writing    -> empty   (E is being written right this moment)
 *   (s>>1) == E, done       -> seqlock-read the payload, re-check the stamp is
 *                              unchanged -> ok
 *   (s>>1)  > E             -> lapped: the producer overwrote E; resync forward to
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

#include "sequencer.hpp" // cache_line
#include "slots.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ufw::core {

enum class read_status : std::uint8_t { ok, empty, lapped };

// Independent, registry-free reader over a multicast_buffer. Detects when the
// producer has lapped it and resyncs forward; move-only (copying would alias a
// position with no benefit).
template <class T>
class buffer_reader
{
public:
    buffer_reader(T const* slots, std::atomic<std::uint64_t> const* stamps,
                  std::atomic<std::uint64_t> const* produce_pos,
                  std::uint64_t mask, std::uint64_t start_pos) noexcept:
        slots_{slots}, stamps_{stamps}, produce_pos_{produce_pos},
        mask_{mask}, read_pos_{start_pos}
    {
    }

    ~buffer_reader() = default;
    buffer_reader(buffer_reader&&) noexcept = default;
    buffer_reader& operator=(buffer_reader&&) noexcept = default;
    buffer_reader(buffer_reader const&) = delete;
    buffer_reader& operator=(buffer_reader const&) = delete;

    // Try to read the next record. ok: `out` is set and the position advanced by 1.
    // empty: nothing new (or being written). lapped: the producer overwrote records
    // before we reached them — `out` is untouched, the reader has resynced to the
    // oldest still-readable position, and `skipped` is how many records were lost.
    [[nodiscard]] read_status try_read(T& out, std::uint64_t& skipped) noexcept
    {
        std::uint64_t const slot = read_pos_ & mask_;
        std::uint64_t const s0 = stamps_[slot].load(std::memory_order_acquire);
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
        // stable at E: seqlock read, then re-check the stamp did not move.
        out = slots_[slot];
        std::atomic_thread_fence(std::memory_order_acquire);
        std::uint64_t const s1 = stamps_[slot].load(std::memory_order_relaxed);
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

    T const*                          slots_;
    std::atomic<std::uint64_t> const* stamps_;
    std::atomic<std::uint64_t> const* produce_pos_;
    std::uint64_t                     mask_;
    std::uint64_t                     read_pos_;
};

template <class T>
class multicast_buffer
{
public:
    explicit multicast_buffer(std::size_t min_slots):
        payloads_{min_slots},
        stamps_{std::make_unique<std::atomic<std::uint64_t>[]>(payloads_.capacity())} // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    {
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return payloads_.capacity(); }

    // Producer side (one thread). Never blocks; overwrites the oldest slot.
    void push(T const& value) noexcept
    {
        std::uint64_t const pos  = produce_pos_.load(std::memory_order_relaxed); // producer-local
        std::uint64_t const slot = pos & mask();
        stamps_[slot].store((pos << 1) | 1U, std::memory_order_relaxed); // writing (odd)
        std::atomic_thread_fence(std::memory_order_release);
        *payloads_.slot(pos) = value;                                    // overwrite payload
        stamps_[slot].store(pos << 1, std::memory_order_release);        // done (even)
        produce_pos_.store(pos + 1, std::memory_order_release);          // publish the live edge
    }

    // Hand out an independent reader, starting at the first position (1). If the
    // producer has already lapped past it, the first read reports a gap and resyncs
    // to the oldest still-readable record. (1-based: position 0 is the sentinel.)
    [[nodiscard]] buffer_reader<T> subscribe() const noexcept
    {
        return buffer_reader<T>{payloads_.data(), stamps_.get(), &produce_pos_, mask(), 1};
    }

private:
    [[nodiscard]] std::uint64_t mask() const noexcept { return payloads_.capacity() - 1; }

    slot_storage<T> payloads_;
    // Runtime-sized array of atomics: vector<atomic> can't reallocate (atomics don't
    // move) and std::array needs a compile-time size, so unique_ptr<T[]> is the fit.
    // (Co-locating each stamp with its payload in one cache line is a future tuning.)
    std::unique_ptr<std::atomic<std::uint64_t>[]> stamps_; // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    alignas(cache_line) std::atomic<std::uint64_t> produce_pos_{1}; // next position to write; published
};

} // namespace ufw::core
