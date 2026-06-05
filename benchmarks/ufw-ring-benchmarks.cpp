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

#if defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#elif defined(__APPLE__)
#  include <pthread.h>
#  include <sys/qos.h>
#endif

namespace {

using ufw::core::multicast_feed;
using ufw::core::multicast_channel;
using ufw::core::spsc_ring;
using ufw::core::multicast_gate;
using ufw::core::lazy_min_gate;
using ufw::core::padded_sequence;

// Best-effort thread placement for steadier benchmark numbers. On Linux this is a
// HARD pin to logical CPU `core` (deterministic). On macOS there is NO per-core
// affinity API — Apple Silicon ignores THREAD_AFFINITY_POLICY entirely — so the best
// we can do is bias onto the performance (P) cores via QoS, keeping bench threads
// off the efficiency (E) cores; `core` is ignored there. For truly deterministic
// per-core pinning, run the suite on Linux.
void pin_thread([[maybe_unused]] unsigned core) noexcept
{
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

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
    pin_thread(0);
    spsc_ring<std::uint64_t> ring{1 << 16};
    std::atomic<bool> stop{false};
    std::thread producer([&ring, &stop]
    {
        pin_thread(1);
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
    pin_thread(0);
    auto const batch = static_cast<std::uint64_t>(state.range(0));
    spsc_ring<std::uint64_t> ring{1 << 16};
    std::atomic<bool> stop{false};
    std::thread producer([&ring, &stop, batch]
    {
        pin_thread(1);
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
    pin_thread(0);
    constexpr std::uint64_t stop_token = ~std::uint64_t{0};
    spsc_ring<std::uint64_t> a2b{1024};
    spsc_ring<std::uint64_t> b2a{1024};

    std::thread responder([&a2b, &b2a]
    {
        pin_thread(1);
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
    pin_thread(0);
    auto const subscribers = static_cast<std::size_t>(state.range(0));
    multicast_channel<std::uint64_t> ring{1 << 16, subscribers};
    std::atomic<bool> stop{false};

    std::vector<std::thread> readers;
    for (std::size_t s = 0; s < subscribers; ++s)
    {
        readers.emplace_back([&ring, &stop, s]
        {
            pin_thread(static_cast<unsigned>(2 + s));
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

// Multicast throughput with BATCH on both sides: the producer bench loop claims/
// fills/commits in runs of 64 (gated by the slowest of N), and each subscriber
// peeks a contiguous run and releases it once. Coarser releases advance the gate
// (min of N read cursors) in bigger jumps. Compare items/sec to the scalar
// ring_channel_throughput. NB on a large (unsaturated) ring the producer is rarely
// gated, so this mostly isolates the producer's commit amortization.
void ring_channel_batch_throughput(benchmark::State& state)
{
    pin_thread(0);
    auto const subscribers = static_cast<std::size_t>(state.range(0));
    multicast_channel<std::uint64_t> ring{1 << 16, subscribers};
    std::atomic<bool> stop{false};

    std::vector<std::thread> readers;
    for (std::size_t s = 0; s < subscribers; ++s)
    {
        readers.emplace_back([&ring, &stop, s]
        {
            pin_thread(static_cast<unsigned>(2 + s));
            auto sub = ring.subscribe();
            while (!stop.load(std::memory_order_relaxed))
            {
                std::span<std::uint64_t const> const run = sub.peek_batch();
                for (std::uint64_t v : run)
                {
                    benchmark::DoNotOptimize(v);
                }
                if (!run.empty())
                {
                    sub.release(run.size());
                }
            }
        });
    }

    std::span<std::uint64_t> claim{};
    std::size_t idx = 0;
    std::uint64_t v = 0;
    std::uint64_t pushed = 0;
    for (auto _ : state)
    {
        if (idx == claim.size())
        {
            if (idx != 0)
            {
                ring.commit(); // publish the whole filled run at once
            }
            while ((claim = ring.try_claim_batch(64)).empty()) { /* spin: gated by slowest */ }
            idx = 0;
        }
        claim[idx] = v++;
        ++idx;
        ++pushed;
    }
    if (idx != 0)
    {
        ring.commit();
    }
    stop.store(true);
    for (auto& t : readers)
    {
        t.join();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(pushed));
}
BENCHMARK(ring_channel_batch_throughput)->Arg(1)->Arg(4)->UseRealTime();

// Multicast per-subscriber DRAIN rate: a producer thread feeds a 1-subscriber
// multicast ring; the benchmark loop is the subscriber. Same ROLE as
// ring_spsc_throughput (a reader fed by a racing producer), so it is the fair
// apples-to-apples — it isolates the reader<T> cost from the producer-gated,
// lockstep fan-out measured above.
void ring_channel_drain(benchmark::State& state)
{
    pin_thread(0);
    multicast_channel<std::uint64_t> ring{1 << 16, 1};
    auto sub = ring.subscribe();
    std::atomic<bool> stop{false};
    std::thread producer([&ring, &stop]
    {
        pin_thread(1);
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

// Multicast per-subscriber BATCH drain: like ring_channel_drain but BOTH sides
// batch — a producer thread claims/fills/commits in runs (so the consumer builds a
// real backlog), and the subscriber bench loop peeks a contiguous run and releases
// it once. Shows the multicast batch path vs the scalar one-at-a-time drain.
void ring_channel_batch_drain(benchmark::State& state)
{
    pin_thread(0);
    multicast_channel<std::uint64_t> ring{1 << 16, 1};
    auto sub = ring.subscribe();
    std::atomic<bool> stop{false};
    std::thread producer([&ring, &stop]
    {
        pin_thread(1);
        std::uint64_t v = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            std::span<std::uint64_t> const claim = ring.try_claim_batch(256);
            for (std::uint64_t& slot : claim)
            {
                slot = v++;
            }
            if (!claim.empty())
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
                sub.release(idx);
            }
            while ((run = sub.peek_batch()).empty()) { /* spin */ }
            idx = 0;
        }
        std::uint64_t out = run[idx];
        benchmark::DoNotOptimize(out);
        ++idx;
        ++got;
    }
    if (idx != 0)
    {
        sub.release(idx);
    }
    stop.store(true);
    producer.join();
    state.SetItemsProcessed(static_cast<std::int64_t>(got));
}
BENCHMARK(ring_channel_batch_drain)->UseRealTime();

// Lossy multicast (multicast_feed): the producer NEVER waits — no gate, no
// subscriber registry — it just overwrites and stamps (per-slot seqlock = 2 stamp
// stores + payload). The Arg is the number of background subscriber threads
// draining concurrently. The whole point: the producer's push rate is ~constant
// across 0/1/4 subscribers (contrast ring_channel_throughput, which the gate
// drags down as subscribers are added).
void ring_feed_push(benchmark::State& state)
{
    pin_thread(0);
    auto const subscribers = static_cast<std::size_t>(state.range(0));
    multicast_feed<std::uint64_t> buf{1 << 16};
    std::atomic<bool> stop{false};

    std::vector<std::thread> readers;
    for (std::size_t s = 0; s < subscribers; ++s)
    {
        readers.emplace_back([&buf, &stop, s]
        {
            pin_thread(static_cast<unsigned>(2 + s));
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
// Benchmark                                          Time             CPU   Iterations UserCounters...
// ----------------------------------------------------------------------------------------------------
// ring_push_pop                                   1.82 ns         1.82 ns    359802828 items_per_second=548.743M/s
// ring_spsc_throughput/real_time                  1.89 ns         1.89 ns    453612577 items_per_second=527.775M/s
// ring_spsc_batch_throughput/1/real_time          19.5 ns         19.5 ns     37565824 items_per_second=51.3898M/s
// ring_spsc_batch_throughput/8/real_time          3.78 ns         3.78 ns    180865740 items_per_second=264.73M/s
// ring_spsc_batch_throughput/64/real_time        0.576 ns        0.576 ns   1000000000 items_per_second=1.73684G/s
// ring_spsc_batch_throughput/256/real_time       0.403 ns        0.403 ns   1709108164 items_per_second=2.4797G/s
// ring_pingpong_latency/real_time                 83.8 ns         83.8 ns      8429385 items_per_second=11.933M/s
// ring_channel_throughput/1/real_time             18.8 ns         18.8 ns     37412048 items_per_second=53.2103M/s
// ring_channel_throughput/4/real_time             34.3 ns         34.3 ns     20254637 items_per_second=29.1664M/s
// ring_channel_batch_throughput/1/real_time      0.720 ns        0.720 ns    970932150 items_per_second=1.38794G/s
// ring_channel_batch_throughput/4/real_time       1.11 ns         1.11 ns    624282354 items_per_second=903.405M/s
// ring_channel_drain/real_time                    7.89 ns         7.89 ns     77038953 items_per_second=126.705M/s
// ring_channel_batch_drain/real_time             0.399 ns        0.399 ns   1765176845 items_per_second=2.5035G/s
// ring_feed_push/0/real_time                      1.79 ns         1.79 ns    390495163 items_per_second=558.418M/s
// ring_feed_push/1/real_time                      3.02 ns         3.02 ns    222186278 items_per_second=331.372M/s
// ring_feed_push/4/real_time                      4.92 ns         4.92 ns    146291574 items_per_second=203.062M/s
// ring_gate_laggard<multicast_gate>/2            0.648 ns        0.648 ns   1051035270 items_per_second=1.54431G/s
// ring_gate_laggard<multicast_gate>/4             1.58 ns         1.58 ns    440853240 items_per_second=634.014M/s
// ring_gate_laggard<multicast_gate>/8             2.76 ns         2.76 ns    259963085 items_per_second=362.335M/s
// ring_gate_laggard<multicast_gate>/16            5.89 ns         5.89 ns    120704223 items_per_second=169.731M/s
// ring_gate_laggard<multicast_gate>/32            12.9 ns         12.9 ns     53971950 items_per_second=77.5952M/s
// ring_gate_laggard<lazy_min_gate>/2             0.324 ns        0.324 ns   2165071215 items_per_second=3.08968G/s
// ring_gate_laggard<lazy_min_gate>/4             0.319 ns        0.319 ns   2180033386 items_per_second=3.13691G/s
// ring_gate_laggard<lazy_min_gate>/8             0.321 ns        0.321 ns   2165560169 items_per_second=3.11628G/s
// ring_gate_laggard<lazy_min_gate>/16            0.387 ns        0.387 ns   2180597047 items_per_second=2.58287G/s
// ring_gate_laggard<lazy_min_gate>/32            0.354 ns        0.354 ns   2154800896 items_per_second=2.82838G/s
// ring_gate_lockstep<multicast_gate>/2           0.808 ns        0.808 ns    870289558 items_per_second=1.23768G/s
// ring_gate_lockstep<multicast_gate>/4            2.79 ns         2.79 ns    249506334 items_per_second=358.563M/s
// ring_gate_lockstep<multicast_gate>/8            5.05 ns         5.05 ns    146383797 items_per_second=198.025M/s
// ring_gate_lockstep<multicast_gate>/16           8.86 ns         8.86 ns     79157761 items_per_second=112.85M/s
// ring_gate_lockstep<multicast_gate>/32           19.1 ns         19.1 ns     36876070 items_per_second=52.4569M/s
// ring_gate_lockstep<lazy_min_gate>/2             1.38 ns         1.38 ns    507393447 items_per_second=725.855M/s
// ring_gate_lockstep<lazy_min_gate>/4             3.33 ns         3.32 ns    215793627 items_per_second=300.762M/s
// ring_gate_lockstep<lazy_min_gate>/8             5.36 ns         5.36 ns    129335033 items_per_second=186.705M/s
// ring_gate_lockstep<lazy_min_gate>/16            11.9 ns         11.9 ns     60331305 items_per_second=84.1571M/s
// ring_gate_lockstep<lazy_min_gate>/32            26.3 ns         26.3 ns     26769871 items_per_second=38.0384M/s
// <<<END BENCHMARK RESULTS>>>
