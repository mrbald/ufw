/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 */
#include <ufw/core/metrics/metrics.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ufw::core::metrics {

namespace {

constexpr std::size_t align64(std::size_t n) noexcept
{
    return (n + 63) & ~std::size_t{63};
}

} // namespace

registry::registry(options opts):
    region_{[&opts]
    {
        std::size_t const bytes = align64(sizeof(file_header)
                                          + (std::size_t{opts.max_entries} * sizeof(file_entry)))
                                  + opts.arena_bytes;
        region_options region_opts{.bytes = bytes};
        if (opts.file != nullptr)
        {
            region_opts.file = opts.file;
            region_opts.open_mode = file_mode::create_or_replace;
        }
        return memory_region{region_opts};
    }()},
    header_{reinterpret_cast<file_header*>(region_.data())},
    entries_{reinterpret_cast<file_entry*>(region_.data() + sizeof(file_header))}
{
    std::memset(region_.data(), 0, sizeof(file_header));
    header_->magic          = file_magic;
    header_->version        = file_version;
    header_->entry_capacity = opts.max_entries;
    header_->entry_count    = 0;
    header_->arena_offset   = align64(sizeof(file_header)
                                      + (std::size_t{opts.max_entries} * sizeof(file_entry)));
    header_->arena_capacity = region_.size() - header_->arena_offset;
    header_->arena_used     = 0;
}

std::uint32_t registry::size() const noexcept
{
    return header_->entry_count;
}

// Single registering thread (the structure-locked init phase). The entry becomes
// visible to concurrent OUT-OF-PROCESS readers only via the release bump of
// entry_count, after the entry and its zeroed payload are complete.
std::byte* registry::allocate(std::string_view name, gauge_type type, std::size_t payload_bytes)
{
    if (name.empty() || name.size() >= name_capacity)
    {
        throw std::length_error("metrics: gauge name must be 1.." + std::to_string(name_capacity - 1)
                                + " chars: " + std::string{name});
    }
    for (std::uint32_t i = 0; i < header_->entry_count; ++i)
    {
        if (name == entries_[i].name.data())
        {
            throw std::invalid_argument("metrics: duplicate gauge name: " + std::string{name});
        }
    }
    if (header_->entry_count >= header_->entry_capacity)
    {
        throw std::length_error("metrics: entry capacity exhausted (max_entries)");
    }
    std::size_t const offset = align64(header_->arena_used);
    if (offset + payload_bytes > header_->arena_capacity)
    {
        throw std::length_error("metrics: arena exhausted (arena_bytes)");
    }

    file_entry& entry = entries_[header_->entry_count];
    entry.name.fill('\0');
    std::memcpy(entry.name.data(), name.data(), name.size());
    entry.type           = static_cast<std::uint32_t>(type);
    entry.payload_len    = static_cast<std::uint32_t>(payload_bytes);
    entry.payload_offset = header_->arena_offset + offset;
    header_->arena_used  = offset + payload_bytes;

    std::byte* const payload = region_.data() + entry.payload_offset;
    std::memset(payload, 0, payload_bytes);

    std::atomic_ref{header_->entry_count}
        .store(header_->entry_count + 1, std::memory_order_release);
    return payload;
}

counter registry::make_counter(std::string_view name)
{
    return counter{reinterpret_cast<std::uint64_t*>(
        allocate(name, gauge_type::counter, sizeof(std::uint64_t)))};
}

value<std::int64_t> registry::make_value_i64(std::string_view name)
{
    return value<std::int64_t>{reinterpret_cast<std::int64_t*>(
        allocate(name, gauge_type::value_i64, sizeof(std::int64_t)))};
}

value<double> registry::make_value_f64(std::string_view name)
{
    return value<double>{reinterpret_cast<double*>(
        allocate(name, gauge_type::value_f64, sizeof(double)))};
}

value_str registry::make_value_str(std::string_view name)
{
    return value_str{reinterpret_cast<value_str_payload*>(
        allocate(name, gauge_type::value_str, sizeof(value_str_payload)))};
}

series registry::make_series(std::string_view name)
{
    return series{reinterpret_cast<series_payload*>(
        allocate(name, gauge_type::series, sizeof(series_payload)))};
}

// The writer-side seqlock follows the in-tree multicast_feed pattern exactly:
// mark odd (relaxed) -> release fence -> plain payload writes -> mark even (release).

void value_str::set(std::string_view v) const noexcept
{
    if (p_ == nullptr)
    {
        return;
    }
    std::atomic_ref const seq{p_->seq};
    auto const s0 = seq.load(std::memory_order_relaxed);
    seq.store(s0 + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    std::size_t const n = std::min(v.size(), value_str_capacity - 1);
    p_->chars.fill('\0');
    std::memcpy(p_->chars.data(), v.data(), n);

    seq.store(s0 + 2, std::memory_order_release);
}

void series::record(std::uint64_t sample, std::uint64_t tick_stamp) const noexcept
{
    if (p_ == nullptr)
    {
        return;
    }
    series_payload& s = *p_;
    std::atomic_ref const seq{s.seq};
    auto const s0 = seq.load(std::memory_order_relaxed);
    seq.store(s0 + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    ++s.count;
    s.sum += sample;
    auto const x = static_cast<double>(sample);
    if (s.count == 1)
    {
        s.min  = sample;
        s.max  = sample;
        s.mean = x;
        s.m2   = 0.0;
        s.ewma = x;
    }
    else
    {
        s.min = std::min(s.min, sample);
        s.max = std::max(s.max, sample);
        double const delta = x - s.mean;
        s.mean += delta / static_cast<double>(s.count);
        s.m2   += delta * (x - s.mean);          // Welford: var = m2 / count
        s.ewma += (x - s.ewma) / 16.0;           // alpha = 1/16 per sample
    }
    ++s.buckets.at(hist_bucket_index(sample)); // index proven < hist_buckets (static_assert)

    std::size_t lowest = 0; // worst-N: replace the smallest retained sample
    for (std::size_t i = 1; i < worst_capacity; ++i)
    {
        if (s.worst_vals.at(i) < s.worst_vals.at(lowest))
        {
            lowest = i;
        }
    }
    if (sample > s.worst_vals.at(lowest))
    {
        s.worst_vals.at(lowest)  = sample;
        s.worst_ticks.at(lowest) = tick_stamp;
    }

    seq.store(s0 + 2, std::memory_order_release);
}

} // namespace ufw::core::metrics
