/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * The telemetry substrate: gauges living in a memory-mapped FILE, written inline
 * by the hot path (mmap pages are memory — no syscalls), read by anything that can
 * map the file: tools/metrics.py (dump/watch today, a prometheus-exposition
 * sidecar later), post-mortem after a crash, or another process while we run.
 *
 * The contract per gauge is SINGLE WRITER, many readers:
 *   counter     cumulative monotonic u64 (rates are derived DOWNSTREAM, PromQL-style);
 *   value       instantaneous i64 / f64 / fixed string;
 *   series      u64 samples -> count/sum/min/max + Welford mean/std + EWMA + an
 *               HDR-style log-linear histogram (cumulative bucket counts, mappable
 *               1:1 to prometheus le-buckets / native histograms) + the worst-8
 *               samples with tick stamps (≈ exemplars).
 * Multi-word payloads (string values, series) are guarded by a per-gauge seqlock
 * (even = stable, odd = mid-write) so out-of-process readers get torn-free
 * snapshots; single-u64 payloads are plain atomic reads. All writer-side atomics
 * are std::atomic_ref over the mapped bytes — relaxed single-writer stores, no RMW
 * on the hot path.
 *
 * Registration happens in the structure-locked init phase, single-threaded, fixed
 * capacity, zero allocation afterwards. Names follow the prometheus convention
 * (`ufw_dispatch_latency_ns{worker="1"}`) so exporters pass them through verbatim.
 * The on-file layout is versioned; tools/metrics.py mirrors it field for field.
 */
#pragma once

#include <ufw/core/mem/memory_region.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ufw::core::metrics {

// ---------------------------------------------------------------- on-file layout

inline constexpr std::uint64_t file_magic   = 0x5854454D'57465500ULL; // "\0UFWMETX" LE
inline constexpr std::uint32_t file_version = 1;

struct file_header
{
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t entry_capacity;
    std::uint32_t entry_count; // atomic_ref'd: bumped with release per registration
    std::uint32_t reserved;
    std::uint64_t arena_offset;
    std::uint64_t arena_capacity;
    std::uint64_t arena_used;
    std::array<std::byte, 16> pad;
};
static_assert(sizeof(file_header) == 64);

enum class gauge_type : std::uint8_t
{
    counter   = 1,
    value_i64 = 2,
    value_f64 = 3,
    value_str = 4,
    series    = 5,
};

inline constexpr std::size_t name_capacity = 112; // NUL-terminated prometheus-style name

struct file_entry
{
    std::array<char, name_capacity> name; // NUL-terminated
    std::uint32_t type;                   // gauge_type widened to the wire width
    std::uint32_t payload_len;
    std::uint64_t payload_offset;         // from file start, 64-byte aligned
};
static_assert(sizeof(file_entry) == 128);

// Histogram geometry: log-linear (HDR-style). Values < 16 index linearly; above,
// major = floor(log2(v)), minor = the next 4 bits => relative error <= 1/16.
// Cumulative counts; tools/metrics.py reconstructs the bucket edges from the same
// formulas. Widening `hist_minor_bits` is the resolution upgrade path.
inline constexpr unsigned    hist_minor_bits = 4;
inline constexpr std::size_t hist_minors     = 1U << hist_minor_bits;     // 16
inline constexpr std::size_t hist_buckets    = (64 - hist_minor_bits + 1) * hist_minors; // 976
inline constexpr std::size_t worst_capacity  = 8;

[[nodiscard]] constexpr std::size_t hist_bucket_index(std::uint64_t v) noexcept
{
    if (v < hist_minors)
    {
        return static_cast<std::size_t>(v);
    }
    auto const major = static_cast<unsigned>(std::bit_width(v) - 1);        // >= hist_minor_bits
    auto const minor = static_cast<std::size_t>(v >> (major - hist_minor_bits)) & (hist_minors - 1);
    return ((major - (hist_minor_bits - 1)) * hist_minors) + minor;
}
static_assert(hist_bucket_index(~std::uint64_t{0}) < hist_buckets);

struct series_payload
{
    std::uint64_t seq;   // seqlock: odd while mid-write
    std::uint64_t count;
    std::uint64_t sum;
    std::uint64_t min;
    std::uint64_t max;
    double        mean;  // Welford
    double        m2;    //   var = m2 / count
    double        ewma;  // alpha = 1/16 per sample
    std::array<std::uint64_t, worst_capacity> worst_vals;  // descending-ish, replace-min
    std::array<std::uint64_t, worst_capacity> worst_ticks; // now_ticks() of each worst sample
    std::array<std::uint64_t, hist_buckets>   buckets;     // cumulative counts
};
static_assert(std::is_trivially_copyable_v<series_payload>);

inline constexpr std::size_t value_str_capacity = 56;

struct value_str_payload
{
    std::uint64_t seq; // seqlock (multi-word)
    std::array<char, value_str_capacity> chars; // NUL-terminated
};
static_assert(sizeof(value_str_payload) == 64);

// ---------------------------------------------------------------- write handles
// Trivially-copyable pointers into the mapped file; null-constructed handles are
// inert no-ops so telemetry can be wired unconditionally and enabled by config.

class counter
{
public:
    counter() = default;
    explicit counter(std::uint64_t* cell) noexcept: cell_{cell} {}

    void add(std::uint64_t n = 1) const noexcept
    {
        if (cell_ == nullptr)
        {
            return;
        }
        std::atomic_ref const cell{*cell_};
        cell.store(cell.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
    // Mirror an external cumulative-monotonic source (e.g. worker_stats fields, CPU
    // ns) — the SOURCE guarantees monotonicity, the sampler just reflects it.
    void set(std::uint64_t v) const noexcept
    {
        if (cell_ != nullptr)
        {
            std::atomic_ref{*cell_}.store(v, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] explicit operator bool() const noexcept { return cell_ != nullptr; }

private:
    std::uint64_t* cell_ = nullptr;
};

template <class T> // T in {std::int64_t, double}
class value
{
public:
    value() = default;
    explicit value(T* cell) noexcept: cell_{cell} {}

    void set(T v) const noexcept
    {
        if (cell_ != nullptr)
        {
            std::atomic_ref{*cell_}.store(v, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] explicit operator bool() const noexcept { return cell_ != nullptr; }

private:
    T* cell_ = nullptr;
};

class value_str
{
public:
    value_str() = default;
    explicit value_str(value_str_payload* p) noexcept: p_{p} {}

    void set(std::string_view v) const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return p_ != nullptr; }

private:
    value_str_payload* p_ = nullptr;
};

class series
{
public:
    series() = default;
    explicit series(series_payload* p) noexcept: p_{p} {}

    // Record one sample (e.g. a latency in ns) with its tick stamp for the
    // worst-N exemplars. Single writer; seqlock-guarded against readers.
    void record(std::uint64_t sample, std::uint64_t tick_stamp) const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return p_ != nullptr; }

private:
    series_payload* p_ = nullptr;
};

// ---------------------------------------------------------------- the registry

class registry
{
public:
    struct options
    {
        char const* file = nullptr;       // null => anonymous (tests); else MAP_SHARED file
        std::uint32_t max_entries = 256;
        std::size_t arena_bytes = 8U << 20U; // 8 MiB default (a series payload is ~8.3 KiB)
    };

    explicit registry(options opts);

    // Registration: structure-locked init phase, single (registering) thread.
    // Throws std::length_error / std::invalid_argument / std::runtime_error on
    // capacity exhaustion, oversize or duplicate names.
    [[nodiscard]] counter            make_counter(std::string_view name);
    [[nodiscard]] value<std::int64_t> make_value_i64(std::string_view name);
    [[nodiscard]] value<double>       make_value_f64(std::string_view name);
    [[nodiscard]] value_str           make_value_str(std::string_view name);
    [[nodiscard]] series              make_series(std::string_view name);

    [[nodiscard]] std::uint32_t size() const noexcept;
    [[nodiscard]] memory_region const& region() const noexcept { return region_; }

private:
    [[nodiscard]] std::byte* allocate(std::string_view name, gauge_type type, std::size_t payload_bytes);

    memory_region region_;
    file_header*  header_ = nullptr;
    file_entry*   entries_ = nullptr;
};

} // namespace ufw::core::metrics
