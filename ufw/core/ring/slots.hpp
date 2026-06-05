/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * The typed-storage layer shared by every typed ring: a power-of-two array of T
 * over a memory_region, and an independent sequential reader over it. The
 * lock-free cursor protocol itself lives in sequencer.hpp; this is "what the
 * cursors point at" and "how a consumer walks the log".
 */
#pragma once

#include <ufw/core/mem/memory_region.hpp>

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ufw::core {

// Power-of-two array of T over a memory_region. Absolute positions map to slots
// by masking, so the ring never needs to special-case the wrap.
template <class T>
class slot_storage
{
    static_assert(std::is_trivially_copyable_v<T>, "ring storage requires a trivially-copyable T");

public:
    explicit slot_storage(std::size_t min_slots):
        n_slots_{std::bit_ceil(min_slots < 1 ? std::size_t{1} : min_slots)},
        mask_{n_slots_ - 1},
        region_{{.bytes = n_slots_ * sizeof(T)}},
        slots_{reinterpret_cast<T*>(region_.data())}
    {
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return n_slots_; }
    [[nodiscard]] T*       slot(std::uint64_t pos) noexcept       { return slots_ + (pos & mask_); }
    [[nodiscard]] T const* slot(std::uint64_t pos) const noexcept { return slots_ + (pos & mask_); }

    // Raw base + mask for an independent reader<T>: both are immutable after
    // construction, so the reader caches them and skips chasing a slot_storage
    // pointer (and reloading the mask) on its hot path.
    [[nodiscard]] T const* data() const noexcept { return slots_; }
    [[nodiscard]] std::uint64_t mask() const noexcept { return mask_; }

private:
    std::size_t   n_slots_;
    std::uint64_t mask_;
    memory_region region_;
    T*            slots_;
};

// An independent sequential reader over a published log: sees every record in
// order, at its own pace, caching the producer cursor and publishing its own read
// position (which feeds the multicast gate). Each multicast subscriber is one of
// these. (SPSC keeps an inline copy of this same logic on direct members — a
// detached reader's pointer indirection costs it ~2x on the hottest path; here the
// subscribers are genuinely separate objects, and caching slots_+mask_ keeps the
// read cheap.) Move-only: copying would alias a cursor.
template <class T>
class reader
{
public:
    reader(slot_storage<T> const& storage,
           std::atomic<std::uint64_t> const& producer_pos,
           std::atomic<std::uint64_t>& cursor) noexcept:
        slots_{storage.data()}, mask_{storage.mask()},
        producer_pos_{&producer_pos}, cursor_{&cursor}
    {
    }

    ~reader() = default;
    reader(reader&&) noexcept = default;
    reader& operator=(reader&&) noexcept = default;
    reader(reader const&) = delete;
    reader& operator=(reader const&) = delete;

    // Next readable record for this reader, or nullptr if caught up. The acquire
    // on the producer cursor makes the producer's record writes visible before we
    // dereference the slot.
    [[nodiscard]] T const* try_peek() noexcept
    {
        if (read_pos_ == cached_producer_)
        {
            cached_producer_ = producer_pos_->load(std::memory_order_acquire);
            if (read_pos_ == cached_producer_)
            {
                return nullptr;
            }
        }
        return slots_ + (read_pos_ & mask_);
    }

    // Batch peek: a contiguous run of all currently-available records, clamped to
    // the ring end. Process them, then release(k) ONCE — amortizing this reader's
    // cursor release store over the run. Because the cursor feeds the multicast
    // gate, a coarser release also lets the producer's back-pressure floor advance
    // in bigger jumps (fewer gate refreshes).
    [[nodiscard]] std::span<T const> peek_batch() noexcept
    {
        if (read_pos_ == cached_producer_)
        {
            cached_producer_ = producer_pos_->load(std::memory_order_acquire);
            if (read_pos_ == cached_producer_)
            {
                return {};
            }
        }
        std::uint64_t const avail = cached_producer_ - read_pos_;
        std::uint64_t const to_wrap = (mask_ + 1) - (read_pos_ & mask_);
        return {slots_ + (read_pos_ & mask_), avail < to_wrap ? avail : to_wrap};
    }

    // Advance past the peeked record(s) and publish progress (release: orders this
    // reader's reads of the slots before the producer may overwrite them).
    void release(std::uint64_t n = 1) noexcept
    {
        read_pos_ += n;
        cursor_->store(read_pos_, std::memory_order_release);
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
    T const*      slots_; // cached storage base (immutable after construction)
    std::uint64_t mask_;  // cached capacity-1 (immutable after construction)
    std::atomic<std::uint64_t> const* producer_pos_; // producer's published position
    std::atomic<std::uint64_t>*       cursor_;        // this reader's published read position (gate input)
    std::uint64_t read_pos_ = 0;
    std::uint64_t cached_producer_ = 0;
};

} // namespace ufw::core
