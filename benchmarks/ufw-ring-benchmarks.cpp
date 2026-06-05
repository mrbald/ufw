/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Stage 1B: fixed-size SPSC ring<T> and multicast ring<T> benchmarks. Latest
 * numbers are at the bottom of this file; regenerate with tools/bench.py ring.
 */
#include <benchmark/benchmark.h>

#include <ufw/core/ring/multicast_feed.hpp>
#include <ufw/core/ring/multicast_channel.hpp>
#include <ufw/core/ring/sequencer.hpp>
#include <ufw/core/ring/spsc_ring.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

namespace {

using ufw::core::multicast_feed;
using ufw::core::multicast_channel;
using ufw::core::spsc_ring;
using ufw::core::multicast_gate;
using ufw::core::lazy_min_gate;
using ufw::core::padded_sequence;

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

// SPSC throughput via the BATCH API: the producer claims/fills/commits up to B at a
// time (one cursor publish per run); the consumer drains whatever is available and
// releases once per run. Arg is the producer's max batch B. /1 isolates the
// consumer's opportunistic batching (producer still commits per item); larger B adds
// producer-side amortization. Compare items/sec against ring_spsc_throughput (the
// scalar, one-publish-per-item path, ~1.9 ns/item).
void ring_spsc_batch_throughput(benchmark::State& state)
{
    auto const batch = static_cast<std::uint64_t>(state.range(0));
    spsc_ring<std::uint64_t> ring{1 << 16};
    std::atomic<bool> stop{false};
    std::thread producer([&ring, &stop, batch]
    {
        std::uint64_t v = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            std::span<std::uint64_t> const run = ring.try_claim_batch(batch);
            for (std::uint64_t& slot : run)
            {
                slot = v++;
            }
            if (!run.empty())
            {
                ring.commit();
            }
        }
    });

    std::span<std::uint64_t const> run{};
    std::size_t idx = 0;
    std::uint64_t got = 0;
    for (auto _ : state)
    {
        if (idx == run.size())
        {
            if (idx != 0)
            {
                ring.release(idx); // publish the whole consumed run at once
            }
            while ((run = ring.peek_batch()).empty()) { /* spin */ }
            idx = 0;
        }
        std::uint64_t out = run[idx];
        benchmark::DoNotOptimize(out);
        ++idx;
        ++got;
    }
    if (idx != 0)
    {
        ring.release(idx);
    }
    stop.store(true);
    producer.join();
    state.SetItemsProcessed(static_cast<std::int64_t>(got));
}
BENCHMARK(ring_spsc_batch_throughput)->Arg(1)->Arg(8)->Arg(64)->Arg(256)->UseRealTime();

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
void ring_channel_throughput(benchmark::State& state)
{
    auto const subscribers = static_cast<std::size_t>(state.range(0));
    multicast_channel<std::uint64_t> ring{1 << 16, subscribers};
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
BENCHMARK(ring_channel_throughput)->Arg(1)->Arg(4)->UseRealTime();

// Multicast per-subscriber DRAIN rate: a producer thread feeds a 1-subscriber
// multicast ring; the benchmark loop is the subscriber. Same ROLE as
// ring_spsc_throughput (a reader fed by a racing producer), so it is the fair
// apples-to-apples — it isolates the reader<T> cost from the producer-gated,
// lockstep fan-out measured above.
void ring_channel_drain(benchmark::State& state)
{
    multicast_channel<std::uint64_t> ring{1 << 16, 1};
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
BENCHMARK(ring_channel_drain)->UseRealTime();

// Lossy multicast (multicast_feed): the producer NEVER waits — no gate, no
// subscriber registry — it just overwrites and stamps (per-slot seqlock = 2 stamp
// stores + payload). The Arg is the number of background subscriber threads
// draining concurrently. The whole point: the producer's push rate is ~constant
// across 0/1/4 subscribers (contrast ring_channel_throughput, which the gate
// drags down as subscribers are added).
void ring_feed_push(benchmark::State& state)
{
    auto const subscribers = static_cast<std::size_t>(state.range(0));
    multicast_feed<std::uint64_t> buf{1 << 16};
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
BENCHMARK(ring_feed_push)->Arg(0)->Arg(1)->Arg(4)->UseRealTime();

// --- gate floor-computation cost, in isolation (single-threaded, no fan-out) ---
// These benchmark ONLY the producer's view of the consumer side: how many cursor
// loads it takes to compute the back-pressure floor. Arg is N (subscriber count).

// PERSISTENT-LAGGARD regime: one slow cursor drags the floor while N-1 sit far
// ahead. lazy_min_gate's good case — it reads ONE cursor; multicast_gate scans all
// N every call. The gap should widen with N.
template <class Gate>
void ring_gate_laggard(benchmark::State& state)
{
    auto const n = static_cast<std::size_t>(state.range(0));
    std::vector<padded_sequence> cur(n);
    for (std::size_t i = 1; i < n; ++i)
    {
        cur[i].value.store(std::uint64_t{1} << 40, std::memory_order_relaxed); // pack far ahead, never caught
    }
    Gate gate{cur.data(), n};
    std::uint64_t v = 0;
    for (auto _ : state)
    {
        cur[0].value.store(++v, std::memory_order_relaxed); // laggard creeps forward, stays the floor
        benchmark::DoNotOptimize(gate.position());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK_TEMPLATE(ring_gate_laggard, multicast_gate)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Arg(32);
BENCHMARK_TEMPLATE(ring_gate_laggard, lazy_min_gate)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Arg(32);

// LOCK-STEP regime: all cursors advance together, so the floor keeps crossing the
// trip and lazy_min_gate rescans every call (a wasted probe + a full scan) — its
// worst case; it should match or slightly trail multicast_gate. The N relaxed
// stores per iter are identical harness overhead for both, so the gate-to-gate
// delta is the signal.
template <class Gate>
void ring_gate_lockstep(benchmark::State& state)
{
    auto const n = static_cast<std::size_t>(state.range(0));
    std::vector<padded_sequence> cur(n);
    Gate gate{cur.data(), n};
    std::uint64_t v = 0;
    for (auto _ : state)
    {
        ++v;
        for (std::size_t i = 0; i < n; ++i)
        {
            cur[i].value.store(v, std::memory_order_relaxed);
        }
        benchmark::DoNotOptimize(gate.position());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK_TEMPLATE(ring_gate_lockstep, multicast_gate)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Arg(32);
BENCHMARK_TEMPLATE(ring_gate_lockstep, lazy_min_gate)->Arg(2)->Arg(4)->Arg(8)->Arg(16)->Arg(32);

} // namespace

// <<<BENCHMARK RESULTS — regenerated by tools/bench.py; do not edit below>>>
// platform: Darwin arm64 | build: build/Release | filter: ring_
// Benchmark                                         Time             CPU   Iterations UserCounters...
// ---------------------------------------------------------------------------------------------------
// ring_push_pop                                  1.86 ns         1.86 ns    374798546 items_per_second=538.245M/s
// ring_spsc_throughput/real_time                 1.93 ns         1.93 ns    355003029 items_per_second=518.574M/s
// ring_spsc_batch_throughput/1/real_time         19.2 ns         19.2 ns     36969838 items_per_second=52.1211M/s
// ring_spsc_batch_throughput/8/real_time         3.82 ns         3.82 ns    184481625 items_per_second=261.766M/s
// ring_spsc_batch_throughput/64/real_time       0.713 ns        0.713 ns   1006995019 items_per_second=1.40182G/s
// ring_spsc_batch_throughput/256/real_time      0.414 ns        0.414 ns   1762891878 items_per_second=2.41718G/s
// ring_pingpong_latency/real_time                82.1 ns         82.1 ns      8807755 items_per_second=12.1786M/s
// ring_channel_throughput/1/real_time            19.3 ns         19.3 ns     36444841 items_per_second=51.9312M/s
// ring_channel_throughput/4/real_time            34.1 ns         34.0 ns     20307772 items_per_second=29.361M/s
// ring_channel_drain/real_time                   9.79 ns         9.79 ns     78792671 items_per_second=102.168M/s
// ring_feed_push/0/real_time                     1.79 ns         1.79 ns    386268966 items_per_second=558.382M/s
// ring_feed_push/1/real_time                     3.26 ns         3.26 ns    192208926 items_per_second=306.623M/s
// ring_feed_push/4/real_time                     5.01 ns         5.01 ns    137346240 items_per_second=199.434M/s
// ring_gate_laggard<multicast_gate>/2           0.656 ns        0.656 ns   1094006408 items_per_second=1.52389G/s
// ring_gate_laggard<multicast_gate>/4            1.54 ns         1.54 ns    449475719 items_per_second=649.14M/s
// ring_gate_laggard<multicast_gate>/8            2.64 ns         2.64 ns    262604057 items_per_second=378.778M/s
// ring_gate_laggard<multicast_gate>/16           5.82 ns         5.82 ns    120904365 items_per_second=171.887M/s
// ring_gate_laggard<multicast_gate>/32           12.9 ns         12.9 ns     54595370 items_per_second=77.3263M/s
// ring_gate_laggard<lazy_min_gate>/2            0.314 ns        0.314 ns   2287260287 items_per_second=3.18557G/s
// ring_gate_laggard<lazy_min_gate>/4            0.312 ns        0.312 ns   2296045554 items_per_second=3.20548G/s
// ring_gate_laggard<lazy_min_gate>/8            0.312 ns        0.312 ns   2252049365 items_per_second=3.20581G/s
// ring_gate_laggard<lazy_min_gate>/16           0.323 ns        0.323 ns   2251368510 items_per_second=3.0919G/s
// ring_gate_laggard<lazy_min_gate>/32           0.313 ns        0.313 ns   2220544478 items_per_second=3.19797G/s
// ring_gate_lockstep<multicast_gate>/2          0.803 ns        0.803 ns    870787565 items_per_second=1.24483G/s
// ring_gate_lockstep<multicast_gate>/4           2.76 ns         2.76 ns    254700127 items_per_second=362.925M/s
// ring_gate_lockstep<multicast_gate>/8           4.78 ns         4.78 ns    147321898 items_per_second=209.308M/s
// ring_gate_lockstep<multicast_gate>/16          9.15 ns         9.15 ns     74183190 items_per_second=109.264M/s
// ring_gate_lockstep<multicast_gate>/32          19.1 ns         19.1 ns     36665532 items_per_second=52.4591M/s
// ring_gate_lockstep<lazy_min_gate>/2            1.39 ns         1.39 ns    503709460 items_per_second=719.512M/s
// ring_gate_lockstep<lazy_min_gate>/4            3.16 ns         3.16 ns    208887258 items_per_second=316.4M/s
// ring_gate_lockstep<lazy_min_gate>/8            5.46 ns         5.46 ns    127404765 items_per_second=183.197M/s
// ring_gate_lockstep<lazy_min_gate>/16           12.1 ns         12.1 ns     57755776 items_per_second=82.9712M/s
// ring_gate_lockstep<lazy_min_gate>/32           26.5 ns         26.5 ns     26463327 items_per_second=37.7419M/s
// <<<END BENCHMARK RESULTS>>>
