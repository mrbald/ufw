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
#include <ufw/core/sys/affinity.hpp>

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
using ufw::core::pin_thread;

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

// In-situ gate comparison under sustained back-pressure (the "A->B pipeline"): a
// SMALL multicast ring with N subscribers, ONE a persistent laggard (a little fixed
// work per item) that the producer is gated by. The producer bench loop spins on
// the gate while back-pressured, so every spin reads it — O(N) for multicast_gate
// vs O(1) for lazy_min_gate (the laggard stays the cached floor). Templated on the
// gate; Arg is N. FINDING: most of the isolated gate win (ring_gate_laggard, up to
// 43x) is HIDDEN here — the producer is bounded by the laggard's CONSUME rate, so at
// N=4 both gates measure identically. It only leaks into throughput as fan-out grows
// (~12% at N=8), where the producer spends more time spinning on the larger gate. The
// gate is a spin-CPU / large-fan-out optimization, not a pipeline-throughput lever.
template <class Gate>
void ring_channel_laggard(benchmark::State& state)
{
    pin_thread(0);
    auto const n = static_cast<std::size_t>(state.range(0));
    multicast_channel<std::uint64_t, Gate> ring{256, n}; // small ring -> producer often gated
    std::atomic<bool> stop{false};

    std::vector<std::thread> readers;
    for (std::size_t s = 0; s < n; ++s)
    {
        readers.emplace_back([&ring, &stop, s]
        {
            pin_thread(static_cast<unsigned>(2 + s));
            auto sub = ring.subscribe();
            bool const laggard = (s == 0);
            std::uint64_t v = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                if (sub.try_pop(v) && laggard)
                {
                    for (int k = 0; k < 40; ++k) { benchmark::DoNotOptimize(k); } // s==0 stays the floor
                }
                benchmark::DoNotOptimize(v);
            }
        });
    }

    std::uint64_t v = 0;
    std::uint64_t pushed = 0;
    for (auto _ : state)
    {
        while (!ring.try_push(v)) { /* spin: gated by the laggard; each spin reads the gate */ }
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
BENCHMARK_TEMPLATE(ring_channel_laggard, multicast_gate)->Arg(4)->Arg(8)->UseRealTime();
BENCHMARK_TEMPLATE(ring_channel_laggard, lazy_min_gate)->Arg(4)->Arg(8)->UseRealTime();

} // namespace

// <<<BENCHMARK RESULTS — regenerated by tools/bench.py; do not edit below>>>
// platform: Darwin arm64 | build: build/Release | filter: ring_
// Benchmark                                                 Time             CPU   Iterations UserCounters...
// -----------------------------------------------------------------------------------------------------------
// ring_push_pop                                          1.83 ns         1.83 ns    298121303 items_per_second=546.347M/s
// ring_spsc_throughput/real_time                         1.52 ns         1.52 ns    343168622 items_per_second=659.434M/s
// ring_spsc_batch_throughput/1/real_time                 18.4 ns         18.4 ns     33518642 items_per_second=54.3914M/s
// ring_spsc_batch_throughput/8/real_time                 4.32 ns         4.32 ns    139877607 items_per_second=231.733M/s
// ring_spsc_batch_throughput/64/real_time               0.479 ns        0.479 ns   1000000000 items_per_second=2.08819G/s
// ring_spsc_batch_throughput/256/real_time              0.410 ns        0.410 ns   1385145732 items_per_second=2.43653G/s
// ring_pingpong_latency/real_time                        84.0 ns         84.0 ns      6358479 items_per_second=11.902M/s
// ring_channel_throughput/1/real_time                    9.15 ns         9.15 ns     56921415 items_per_second=109.268M/s
// ring_channel_throughput/4/real_time                    13.7 ns         13.7 ns     39129110 items_per_second=73.117M/s
// ring_channel_batch_throughput/1/real_time             0.728 ns        0.728 ns    782659442 items_per_second=1.37323G/s
// ring_channel_batch_throughput/4/real_time              1.14 ns         1.13 ns    488159942 items_per_second=879.593M/s
// ring_channel_drain/real_time                           8.94 ns         8.94 ns     74614851 items_per_second=111.814M/s
// ring_channel_batch_drain/real_time                    0.419 ns        0.419 ns   1393536014 items_per_second=2.38831G/s
// ring_feed_push/0/real_time                             1.82 ns         1.82 ns    308183722 items_per_second=550.77M/s
// ring_feed_push/1/real_time                             3.29 ns         3.29 ns    151389932 items_per_second=304.249M/s
// ring_feed_push/4/real_time                             5.04 ns         5.04 ns    111647564 items_per_second=198.254M/s
// ring_gate_laggard<multicast_gate>/2                   0.648 ns        0.648 ns    889891783 items_per_second=1.5441G/s
// ring_gate_laggard<multicast_gate>/4                    1.55 ns         1.55 ns    357379895 items_per_second=644.382M/s
// ring_gate_laggard<multicast_gate>/8                    2.70 ns         2.70 ns    202608586 items_per_second=370.412M/s
// ring_gate_laggard<multicast_gate>/16                   5.92 ns         5.92 ns     95288332 items_per_second=168.884M/s
// ring_gate_laggard<multicast_gate>/32                   12.9 ns         12.9 ns     43369165 items_per_second=77.3992M/s
// ring_gate_laggard<lazy_min_gate>/2                    0.319 ns        0.319 ns   1745668560 items_per_second=3.13879G/s
// ring_gate_laggard<lazy_min_gate>/4                    0.323 ns        0.323 ns   1442410535 items_per_second=3.09487G/s
// ring_gate_laggard<lazy_min_gate>/8                    0.319 ns        0.319 ns   1747411646 items_per_second=3.13645G/s
// ring_gate_laggard<lazy_min_gate>/16                   0.318 ns        0.318 ns   1773577496 items_per_second=3.14472G/s
// ring_gate_laggard<lazy_min_gate>/32                   0.323 ns        0.323 ns   1729649592 items_per_second=3.09565G/s
// ring_gate_lockstep<multicast_gate>/2                  0.821 ns        0.820 ns    693361068 items_per_second=1.21884G/s
// ring_gate_lockstep<multicast_gate>/4                   2.85 ns         2.85 ns    193966936 items_per_second=351.432M/s
// ring_gate_lockstep<multicast_gate>/8                   5.09 ns         5.09 ns    106583430 items_per_second=196.448M/s
// ring_gate_lockstep<multicast_gate>/16                  9.66 ns         9.65 ns     57969214 items_per_second=103.63M/s
// ring_gate_lockstep<multicast_gate>/32                  19.1 ns         19.1 ns     29549892 items_per_second=52.4598M/s
// ring_gate_lockstep<lazy_min_gate>/2                    1.41 ns         1.41 ns    395485812 items_per_second=711.295M/s
// ring_gate_lockstep<lazy_min_gate>/4                    3.28 ns         3.28 ns    171909391 items_per_second=305.079M/s
// ring_gate_lockstep<lazy_min_gate>/8                    5.61 ns         5.61 ns    103214391 items_per_second=178.357M/s
// ring_gate_lockstep<lazy_min_gate>/16                   12.2 ns         12.2 ns     45678116 items_per_second=81.8063M/s
// ring_gate_lockstep<lazy_min_gate>/32                   26.6 ns         26.6 ns     21059836 items_per_second=37.5999M/s
// ring_channel_laggard<multicast_gate>/4/real_time       24.5 ns         24.5 ns     22094612 items_per_second=40.8321M/s
// ring_channel_laggard<multicast_gate>/8/real_time       77.1 ns         76.9 ns      7244783 items_per_second=12.9673M/s
// ring_channel_laggard<lazy_min_gate>/4/real_time        24.4 ns         24.4 ns     22169295 items_per_second=41.0353M/s
// ring_channel_laggard<lazy_min_gate>/8/real_time        68.6 ns         68.5 ns      8257171 items_per_second=14.5739M/s
// <<<END BENCHMARK RESULTS>>>
