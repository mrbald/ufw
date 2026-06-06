/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * The dispatch fabric (ufw/core/exec/): the resolved inbox handle, the erased
 * command, the worker/matrix. Latest numbers are at the bottom of this file;
 * regenerate with tools/bench.py dispatch. These feed the plan's stop-and-think
 * gates: G2 (direct call vs a virtual baseline), G1 (empty drain turn cost),
 * G3 (cross-worker round trip / throughput; the slot-size sweep is compile-time —
 * change dispatch_arg_bytes and re-record).
 */
#include <benchmark/benchmark.h>

#include <ufw/core/exec/dispatch_matrix.hpp>
#include <ufw/core/exec/inbox.hpp>
#include <ufw/core/exec/worker.hpp>
#include <ufw/core/sys/affinity.hpp>
#include <ufw/core/sys/cpu.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using ufw::core::column_drainer;
using ufw::core::cpu_relax;
using ufw::core::dispatch_cmd;
using ufw::core::dispatch_matrix;
using ufw::core::inbox_handle;
using ufw::core::loop_kind;
using ufw::core::make_direct_handle;
using ufw::core::make_enqueue_handle;
using ufw::core::pin_thread;
using ufw::core::worker;

struct bench_actor
{
    std::uint64_t acc = 0;
    void on_msg(std::uint64_t v) noexcept { acc += v; }
};

// G2 baseline: the same call through a classic virtual interface.
struct virt_base
{
    virtual void on_msg(std::uint64_t v) noexcept = 0;
    virtual ~virt_base() = default;
    virt_base() = default;
    virt_base(virt_base const&) = delete;
    virt_base& operator=(virt_base const&) = delete;
    virt_base(virt_base&&) = delete;
    virt_base& operator=(virt_base&&) = delete;
};
struct virt_actor final : virt_base
{
    std::uint64_t acc = 0;
    void on_msg(std::uint64_t v) noexcept override { acc += v; }
};

void dispatch_virtual_baseline(benchmark::State& state)
{
    pin_thread(0);
    virt_actor impl;
    virt_base* target = &impl;
    benchmark::DoNotOptimize(target); // hide the dynamic type from the optimizer
    std::uint64_t i = 0;
    for (auto _ : state)
    {
        target->on_msg(i++);
        benchmark::DoNotOptimize(impl.acc);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(dispatch_virtual_baseline);

// G2: the SAME-worker resolved handle — one erased call through the trampoline,
// args packed/unpacked via a stack dispatch_cmd. Compare to the virtual baseline;
// if it regresses beyond ~1-2ns, split a thin direct trampoline into the handle.
void dispatch_direct_call(benchmark::State& state)
{
    pin_thread(0);
    bench_actor target;
    auto const handle = make_direct_handle<&bench_actor::on_msg>(&target);
    std::uint64_t i = 0;
    for (auto _ : state)
    {
        handle(i++);
        benchmark::DoNotOptimize(target.acc);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(dispatch_direct_call);

// G1: the cost of one EMPTY drain turn (the spinning worker's idle overhead per
// inbound cell): peek_batch finds nothing across `Arg` empty cells.
void dispatch_empty_drain_turn(benchmark::State& state)
{
    pin_thread(0);
    auto const producers = static_cast<unsigned>(state.range(0));
    dispatch_matrix matrix{producers + 1, 256};
    bench_actor target;
    // materialize `producers` inbound cells for worker `producers` (the last id)
    std::vector<inbox_handle<void(std::uint64_t)>> handles;
    for (unsigned from = 0; from < producers; ++from)
    {
        handles.push_back(make_enqueue_handle<&bench_actor::on_msg>(
            &target, matrix.cell(from, producers), nullptr));
    }
    ufw::core::worker_stats stats{};
    column_drainer drainer{matrix.inbound(producers), stats};
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(drainer.poll()); // all cells empty
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(dispatch_empty_drain_turn)->Arg(1)->Arg(4);

// Cross-worker ROUND TRIP: this thread (worker 0) pings an echo actor on a
// spinning worker 1 via cell [0][1]; the echo replies into cell [1][0], which this
// thread drains. One iteration = one full round trip through two SPSC cells + two
// trampolines. The dispatch-fabric analogue of ring_pingpong_latency.
void dispatch_cross_worker_rtt(benchmark::State& state)
{
    pin_thread(0);
    dispatch_matrix matrix{2, 1024};

    struct sink
    {
        std::uint64_t last = 0;
        void on_reply(std::uint64_t v) noexcept { last = v; }
    };
    struct echo
    {
        inbox_handle<void(std::uint64_t)> reply;
        void on_ping(std::uint64_t v) noexcept { reply(v); }
    };

    sink s;
    echo e;
    auto const ping = make_enqueue_handle<&echo::on_ping>(&e, matrix.cell(0, 1), nullptr);
    e.reply = make_enqueue_handle<&sink::on_reply>(&s, matrix.cell(1, 0), nullptr);

    worker w1{1, loop_kind::spinning, 2};
    column_drainer drainer{matrix.inbound(1), w1.stats()};
    w1.add_source(drainer);
    w1.launch();

    auto& reply_ring = matrix.cell(1, 0);
    std::uint64_t i = 1;
    for (auto _ : state)
    {
        ping(i);
        for (;;) // drain our own inbound column until the reply lands
        {
            std::span<dispatch_cmd const> const run = reply_ring.peek_batch();
            if (run.empty())
            {
                cpu_relax();
                continue;
            }
            for (dispatch_cmd const& cmd : run)
            {
                cmd.tramp(cmd.obj, cmd);
            }
            reply_ring.release(run.size());
            break;
        }
        benchmark::DoNotOptimize(s.last);
        ++i;
    }
    w1.request_stop();
    w1.join();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(dispatch_cross_worker_rtt)->UseRealTime();

// Cross-worker THROUGHPUT: this thread fires-and-forgets into cell [0][1] as fast
// as the back-pressure allows; a spinning worker 1 drains and executes. The
// dispatch-fabric analogue of ring_spsc_throughput (adds pack + trampoline + stats
// on top of the raw ring).
void dispatch_cross_worker_throughput(benchmark::State& state)
{
    pin_thread(0);
    dispatch_matrix matrix{2, 4096};
    bench_actor target;
    auto const send = make_enqueue_handle<&bench_actor::on_msg>(
        &target, matrix.cell(0, 1), nullptr);

    worker w1{1, loop_kind::spinning, 2};
    column_drainer drainer{matrix.inbound(1), w1.stats()};
    w1.add_source(drainer);
    w1.launch();

    std::uint64_t i = 0;
    for (auto _ : state)
    {
        send(i++); // spin-push inside the handle is the back-pressure
    }
    w1.request_stop();
    w1.join();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(dispatch_cross_worker_throughput)->UseRealTime();

} // namespace

// <<<BENCHMARK RESULTS — regenerated by tools/bench.py; do not edit below>>>
// platform: Darwin arm64 | build: build/Release | filter: dispatch_
// Benchmark                                           Time             CPU   Iterations UserCounters...
// -----------------------------------------------------------------------------------------------------
// dispatch_virtual_baseline                       0.744 ns        0.744 ns    751728304 items_per_second=1.34368G/s
// dispatch_direct_call                            0.283 ns        0.283 ns   2165556819 items_per_second=3.53526G/s
// dispatch_empty_drain_turn/1                      1.74 ns         1.74 ns    322368951 items_per_second=575.818M/s
// dispatch_empty_drain_turn/4                      5.65 ns         5.65 ns     93981808 items_per_second=177.136M/s
// dispatch_cross_worker_rtt/real_time              94.0 ns         94.0 ns      5330858 items_per_second=10.6358M/s
// dispatch_cross_worker_throughput/real_time       25.6 ns         25.6 ns     22249177 items_per_second=39.0464M/s
// <<<END BENCHMARK RESULTS>>>
