/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Stage 1B-i: fixed-size SPSC ring<T> benchmarks. Latest numbers are at the
 * bottom of this file; regenerate with tools/bench.py ring.
 */
#include <benchmark/benchmark.h>

#include <ufw/core/ring/broadcast_ring.hpp>
#include <ufw/core/ring/spmc_ring.hpp>
#include <ufw/core/ring/spsc_ring.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using ufw::core::broadcast_ring;
using ufw::core::spmc_ring;
using ufw::core::spsc_ring;

// Uncontended fast-path cost: one thread pushing then popping (claim/commit +
// peek/release overhead, no cross-core traffic).
void ring_push_pop(benchmark::State& state)
{
    spsc_ring<std::uint64_t> ring{1024};
    std::uint64_t v = 0;
    std::uint64_t out = 0;
    for (auto _ : state)
    {
        ring.try_push(v++);
        ring.try_pop(out);
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(ring_push_pop);

// SPSC throughput: a producer thread feeds; the benchmark loop consumes.
void ring_spsc_throughput(benchmark::State& state)
{
    spsc_ring<std::uint64_t> ring{1 << 16};
    std::atomic<bool> stop{false};
    std::thread producer([&ring, &stop]
    {
        std::uint64_t v = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            if (ring.try_push(v))
            {
                ++v;
            }
        }
    });

    std::uint64_t out = 0;
    std::uint64_t got = 0;
    for (auto _ : state)
    {
        while (!ring.try_pop(out)) { /* spin */ }
        ++got;
        benchmark::DoNotOptimize(out);
    }
    stop.store(true);
    producer.join();
    state.SetItemsProcessed(static_cast<std::int64_t>(got));
}
BENCHMARK(ring_spsc_throughput)->UseRealTime();

// Round-trip latency over two opposed rings (the dialog embryo: a pair of
// opposed pub-subs). One iteration = one request + one reply.
void ring_pingpong_latency(benchmark::State& state)
{
    constexpr std::uint64_t stop_token = ~std::uint64_t{0};
    spsc_ring<std::uint64_t> a2b{1024};
    spsc_ring<std::uint64_t> b2a{1024};

    std::thread responder([&a2b, &b2a]
    {
        std::uint64_t v = 0;
        for (;;)
        {
            while (!a2b.try_pop(v)) { /* spin */ }
            if (v == stop_token)
            {
                break;
            }
            while (!b2a.try_push(v)) { /* spin */ }
        }
    });

    std::uint64_t v = 0;
    std::uint64_t out = 0;
    for (auto _ : state)
    {
        while (!a2b.try_push(v)) { /* spin */ }
        while (!b2a.try_pop(out)) { /* spin */ }
        ++v;
        benchmark::DoNotOptimize(out);
    }
    while (!a2b.try_push(stop_token)) { /* spin */ }
    responder.join();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(ring_pingpong_latency)->UseRealTime();

// SPMC aggregate throughput: the benchmark loop produces; N consumer threads
// drain competitively (each record to exactly one). Measures the steady-state
// rate through the work-sharing ring (and the cost of the min-of-N gate + the
// CAS-claim contention).
void ring_spmc_throughput(benchmark::State& state)
{
    constexpr std::size_t consumers = 4;
    spmc_ring<std::uint64_t> ring{1 << 16, consumers};
    std::atomic<bool> stop{false};

    std::vector<std::thread> pool;
    for (std::size_t i = 0; i < consumers; ++i)
    {
        pool.emplace_back([&ring, &stop, i]
        {
            std::uint64_t v = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                ring.try_consume(i, v);
                benchmark::DoNotOptimize(v);
            }
        });
    }

    std::uint64_t v = 0;
    std::uint64_t pushed = 0;
    for (auto _ : state)
    {
        while (!ring.try_push(v)) { /* spin */ }
        ++v;
        ++pushed;
    }
    stop.store(true);
    for (auto& t : pool)
    {
        t.join();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(pushed));
}
BENCHMARK(ring_spmc_throughput)->UseRealTime();

// Broadcast (pub/sub) throughput: the benchmark loop produces; N subscriber
// threads each read EVERY record. Gated by the slowest subscriber, plus the
// min-of-N gate and producer_pos read by all N — the fan-out cost.
void ring_broadcast_throughput(benchmark::State& state)
{
    constexpr std::size_t subscribers = 4;
    broadcast_ring<std::uint64_t> ring{1 << 16, subscribers};
    std::atomic<bool> stop{false};

    std::vector<std::thread> readers;
    for (std::size_t s = 0; s < subscribers; ++s)
    {
        readers.emplace_back([&ring, &stop]
        {
            auto sub = ring.subscribe();
            std::uint64_t v = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                sub.try_pop(v);
                benchmark::DoNotOptimize(v);
            }
        });
    }

    std::uint64_t v = 0;
    std::uint64_t pushed = 0;
    for (auto _ : state)
    {
        while (!ring.try_push(v)) { /* spin: gated by the slowest subscriber */ }
        ++v;
        ++pushed;
    }
    stop.store(true);
    for (auto& t : readers)
    {
        t.join();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(pushed));
}
BENCHMARK(ring_broadcast_throughput)->UseRealTime();

} // namespace

// <<<BENCHMARK RESULTS — regenerated by tools/bench.py; do not edit below>>>
// platform: Darwin arm64 | build: build/Release | filter: ring_
// Benchmark                                    Time             CPU   Iterations UserCounters...
// ----------------------------------------------------------------------------------------------
// ring_push_pop                             2.15 ns         2.15 ns    132833626 items_per_second=464.855M/s
// ring_spsc_throughput/real_time            3.10 ns         3.10 ns    139199893 items_per_second=322.823M/s
// ring_pingpong_latency/real_time           84.2 ns         84.2 ns      3377658 items_per_second=11.8715M/s
// ring_spmc_throughput/real_time            69.0 ns         68.8 ns      4399847 items_per_second=14.5017M/s
// ring_broadcast_throughput/real_time       34.9 ns         34.8 ns      8640709 items_per_second=28.6798M/s
// <<<END BENCHMARK RESULTS>>>
