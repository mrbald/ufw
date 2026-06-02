/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Stage 1B: fixed-size SPSC ring<T> and multicast ring<T> benchmarks. Latest
 * numbers are at the bottom of this file; regenerate with tools/bench.py ring.
 */
#include <benchmark/benchmark.h>

#include <ufw/core/ring/multicast_buffer.hpp>
#include <ufw/core/ring/multicast_ring.hpp>
#include <ufw/core/ring/spsc_ring.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using ufw::core::multicast_buffer;
using ufw::core::multicast_ring;
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

// Multicast (pub/sub) throughput: the benchmark loop produces; N subscriber
// threads each read EVERY record (gated by the slowest). The Arg is the subscriber
// count: /1 is the apples-to-apples comparison with SPSC — one reader, no fan-out,
// so it should approach ring_spsc_throughput; /4 adds the fan-out + slowest-of-N.
void ring_multicast_throughput(benchmark::State& state)
{
    auto const subscribers = static_cast<std::size_t>(state.range(0));
    multicast_ring<std::uint64_t> ring{1 << 16, subscribers};
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
BENCHMARK(ring_multicast_throughput)->Arg(1)->Arg(4)->UseRealTime();

// Multicast per-subscriber DRAIN rate: a producer thread feeds a 1-subscriber
// multicast ring; the benchmark loop is the subscriber. Same ROLE as
// ring_spsc_throughput (a reader fed by a racing producer), so it is the fair
// apples-to-apples — it isolates the reader<T> cost from the producer-gated,
// lockstep fan-out measured above.
void ring_multicast_drain(benchmark::State& state)
{
    multicast_ring<std::uint64_t> ring{1 << 16, 1};
    auto sub = ring.subscribe();
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
        while (!sub.try_pop(out)) { /* spin */ }
        ++got;
        benchmark::DoNotOptimize(out);
    }
    stop.store(true);
    producer.join();
    state.SetItemsProcessed(static_cast<std::int64_t>(got));
}
BENCHMARK(ring_multicast_drain)->UseRealTime();

// Lossy multicast (multicast_buffer): the producer NEVER waits — no gate, no
// subscriber registry — it just overwrites and stamps (per-slot seqlock = 2 stamp
// stores + payload). The Arg is the number of background subscriber threads
// draining concurrently. The whole point: the producer's push rate is ~constant
// across 0/1/4 subscribers (contrast ring_multicast_throughput, which the gate
// drags down as subscribers are added).
void ring_multicast_buffer_push(benchmark::State& state)
{
    auto const subscribers = static_cast<std::size_t>(state.range(0));
    multicast_buffer<std::uint64_t> buf{1 << 16};
    std::atomic<bool> stop{false};

    std::vector<std::thread> readers;
    for (std::size_t s = 0; s < subscribers; ++s)
    {
        readers.emplace_back([&buf, &stop]
        {
            auto sub = buf.subscribe();
            std::uint64_t out = 0;
            std::uint64_t skipped = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                static_cast<void>(sub.try_read(out, skipped));
                benchmark::DoNotOptimize(out);
            }
        });
    }

    std::uint64_t v = 0;
    for (auto _ : state)
    {
        buf.push(v++);
    }
    stop.store(true);
    for (auto& t : readers)
    {
        t.join();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(ring_multicast_buffer_push)->Arg(0)->Arg(1)->Arg(4)->UseRealTime();

} // namespace

// <<<BENCHMARK RESULTS — regenerated by tools/bench.py; do not edit below>>>
// platform: Darwin arm64 | build: build/Release | filter: ring_
// Benchmark                                       Time             CPU   Iterations UserCounters...
// -------------------------------------------------------------------------------------------------
// ring_push_pop                                1.93 ns         1.93 ns    143128064 items_per_second=517.74M/s
// ring_spsc_throughput/real_time               3.06 ns         3.05 ns    100000000 items_per_second=326.969M/s
// ring_pingpong_latency/real_time              83.8 ns         83.8 ns      3175802 items_per_second=11.9347M/s
// ring_multicast_throughput/1/real_time        17.1 ns         17.1 ns     16643509 items_per_second=58.6047M/s
// ring_multicast_throughput/4/real_time        29.6 ns         29.5 ns      9393858 items_per_second=33.7712M/s
// ring_multicast_drain/real_time               8.01 ns         8.01 ns     36539691 items_per_second=124.813M/s
// ring_multicast_buffer_push/0/real_time       1.86 ns         1.86 ns    150351615 items_per_second=536.484M/s
// ring_multicast_buffer_push/1/real_time       4.08 ns         4.07 ns     62464678 items_per_second=245.395M/s
// ring_multicast_buffer_push/4/real_time       5.84 ns         5.82 ns     50333233 items_per_second=171.236M/s
// <<<END BENCHMARK RESULTS>>>
