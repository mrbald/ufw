/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Dispatch/exec primitives: the timing clock, the erased command, the inbox
 * handle, the worker/dispatch-matrix. Grown tranche by tranche with the actor
 * foundation; tests are the executable spec.
 */
#include <boost/test/unit_test.hpp>

#include <ufw/core/exec/command.hpp>
#include <ufw/core/exec/dispatch_matrix.hpp>
#include <ufw/core/exec/inbox.hpp>
#include <ufw/core/exec/worker.hpp>
#include <ufw/core/ring/spsc_ring.hpp>
#include <ufw/core/sys/cpu.hpp>
#include <ufw/core/sys/thread_usage.hpp>
#include <ufw/core/sys/timing.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>

namespace {

BOOST_AUTO_TEST_SUITE(ufw_core_timing)

BOOST_AUTO_TEST_CASE(now_ticks_is_nondecreasing)
{
    std::uint64_t const a = ufw::core::now_ticks();
    std::uint64_t const b = ufw::core::now_ticks();
    BOOST_TEST(b >= a);
}

BOOST_AUTO_TEST_CASE(ticks_to_ns_roughly_matches_a_sleep)
{
    using namespace std::chrono_literals;
    std::uint64_t const t0 = ufw::core::now_ticks();
    std::this_thread::sleep_for(20ms);
    std::uint64_t const t1 = ufw::core::now_ticks();
    std::uint64_t const ns = ufw::core::ticks_to_ns(t1 - t0);
    BOOST_TEST(ns > 10'000'000U);  // slept at least ~10ms worth of ticks
    BOOST_TEST(ns < 200'000'000U); // and nowhere near 200ms (schedulers oversleep)
}

BOOST_AUTO_TEST_SUITE_END(/* ufw_core_timing */)

BOOST_AUTO_TEST_SUITE(ufw_core_dispatch_cmd)

struct receiver
{
    int got_i = 0;
    double got_d = 0.0;
    void on_msg(int i, double d) noexcept
    {
        got_i = i;
        got_d = d;
    }
};

// A hand-written trampoline — what inbox_handle binds automatically in tranche 3.
void hand_trampoline(void* obj, ufw::core::dispatch_cmd const& cmd) noexcept
{
    ufw::core::apply_from_buffer<int, double>(cmd.args, [obj](int i, double d) noexcept
    {
        static_cast<receiver*>(obj)->on_msg(i, d);
    });
}

BOOST_AUTO_TEST_CASE(command_is_a_valid_one_line_ring_slot)
{
    BOOST_TEST(std::is_trivially_copyable_v<ufw::core::dispatch_cmd>);
    BOOST_TEST(sizeof(ufw::core::dispatch_cmd) == 64U);
}

// Pack a (int, double) "message", run it through the erased trampoline, assert the
// call landed with the right values. (Oversize / non-trivial args are a COMPILE
// error by design — pack_args static_asserts; no runtime test possible or needed.)
BOOST_AUTO_TEST_CASE(pack_then_dispatch_roundtrips_the_args)
{
    receiver r;
    ufw::core::dispatch_cmd cmd{};
    cmd.tramp = &hand_trampoline;
    cmd.obj   = &r;
    cmd.stamp = ufw::core::now_ticks();
    ufw::core::pack_args(cmd.args, 42, 2.5);

    cmd.tramp(cmd.obj, cmd);

    BOOST_TEST(r.got_i == 42);
    BOOST_TEST(r.got_d == 2.5);
}

BOOST_AUTO_TEST_SUITE_END(/* ufw_core_dispatch_cmd */)

BOOST_AUTO_TEST_SUITE(ufw_core_inbox_handle)

struct counter_actor
{
    int sum = 0;
    int calls = 0;
    void on_add(int v) noexcept
    {
        sum += v;
        ++calls;
    }
};

struct counting_wakeable : ufw::core::wakeable
{
    int wakes = 0;
    void wake() noexcept override { ++wakes; }
};

// Same-worker mode: the handle runs the method synchronously, right now.
BOOST_AUTO_TEST_CASE(direct_handle_calls_synchronously)
{
    counter_actor a;
    auto const h = ufw::core::make_direct_handle<&counter_actor::on_add>(&a);
    static_assert(std::is_trivially_copyable_v<std::remove_const_t<decltype(h)>>);
    BOOST_TEST(static_cast<bool>(h));

    h(5);
    h(7);
    BOOST_TEST(a.sum == 12);
    BOOST_TEST(a.calls == 2);
}

// Cross-worker mode: the handle defers into the ring; nothing runs until the
// target side drains it (here: by hand, exactly what a column drainer does).
BOOST_AUTO_TEST_CASE(enqueue_handle_defers_until_the_ring_drains)
{
    counter_actor a;
    ufw::core::spsc_ring<ufw::core::dispatch_cmd> ring{16};
    auto const h = ufw::core::make_enqueue_handle<&counter_actor::on_add>(&a, ring, nullptr);

    h(5);
    h(7);
    BOOST_TEST(a.calls == 0); // deferred — target hasn't drained yet

    std::span<ufw::core::dispatch_cmd const> const run = ring.peek_batch();
    for (ufw::core::dispatch_cmd const& cmd : run)
    {
        cmd.tramp(cmd.obj, cmd);
    }
    ring.release(run.size());

    BOOST_TEST(a.sum == 12);
    BOOST_TEST(a.calls == 2);
}

// The wake hook is wired on the send path from day one (a blocking target parked
// in its backstop gets poked per send; spinners pass null and never branch).
BOOST_AUTO_TEST_CASE(enqueue_handle_wakes_the_target_per_send)
{
    counter_actor a;
    counting_wakeable w;
    ufw::core::spsc_ring<ufw::core::dispatch_cmd> ring{16};
    auto const h = ufw::core::make_enqueue_handle<&counter_actor::on_add>(&a, ring, &w);

    h(1);
    h(2);
    h(3);
    BOOST_TEST(w.wakes == 3);
}

BOOST_AUTO_TEST_SUITE_END(/* ufw_core_inbox_handle */)

BOOST_AUTO_TEST_SUITE(ufw_core_worker_matrix)

struct atomic_actor
{
    std::atomic<int> calls{0};
    std::atomic<long long> sum{0};
    void on_add(int v) noexcept
    {
        sum.fetch_add(v, std::memory_order_relaxed);
        calls.fetch_add(1, std::memory_order_relaxed);
    }
};

// Bounded wait for a cross-thread condition (the tests must terminate).
template <class Cond>
bool await(Cond&& cond)
{
    using clock = std::chrono::steady_clock;
    auto const deadline = clock::now() + std::chrono::seconds(10);
    while (!cond())
    {
        if (clock::now() > deadline)
        {
            return false;
        }
        ufw::core::cpu_relax();
    }
    return true;
}

// The 2-worker spinning matrix, end to end in ufw_core alone: this thread plays
// worker 0 (the single producer of cell [0][1]); worker 1 spins a column drainer.
BOOST_AUTO_TEST_CASE(two_worker_spinning_matrix_dispatches_cross_thread)
{
    constexpr int count = 10'000;
    ufw::core::dispatch_matrix matrix{2, 1024};
    atomic_actor target;

    // wiring phase: resolve creates the 0->1 cell, THEN the drainer snapshots it
    auto const handle = ufw::core::make_enqueue_handle<&atomic_actor::on_add>(
        &target, matrix.cell(0, 1), nullptr);
    ufw::core::worker w1{1, ufw::core::loop_kind::spinning};
    ufw::core::column_drainer drainer{matrix.inbound(1), w1.stats()};
    w1.add_source(drainer);
    w1.launch();

    for (int i = 1; i <= count; ++i)
    {
        handle(i);
    }
    BOOST_TEST(await([&] { return target.calls.load(std::memory_order_relaxed) == count; }));
    w1.request_stop();
    w1.join();

    BOOST_TEST(target.sum.load() == static_cast<long long>(count) * (count + 1) / 2); // FIFO+lossless
    BOOST_TEST(w1.stats().dispatched == static_cast<std::uint64_t>(count));
    BOOST_TEST(w1.stats().useful_iters >= 1U);
    BOOST_TEST(w1.stats().iterations >= w1.stats().useful_iters);
}

// A blocking worker's backstop for the core test: parks on a condvar, wakes on
// wake(). (The real app backstop wraps the asio io_context — tranche 5.)
struct condvar_backstop final : ufw::core::blocking_source
{
    std::mutex m;
    std::condition_variable cv;
    bool poked = false;

    std::size_t poll() noexcept override { return 0; } // nothing pollable here
    std::size_t poll_blocking() noexcept override
    {
        std::unique_lock lock{m};
        cv.wait(lock, [this] { return poked; });
        poked = false;
        return 0; // the park itself runs nothing; the re-drain finds the work
    }
    void wake() noexcept override
    {
        {
            std::lock_guard const lock{m};
            poked = true;
        }
        cv.notify_one();
    }
};

// The deferred blocking-as-matrix-target MECHANISM, proven in core today: the
// send path's day-one `if (notify) notify->wake()` pokes the parked worker, which
// re-drains and runs the command. Shipping the real thing later is only an asio
// backstop with a non-stub wake().
BOOST_AUTO_TEST_CASE(blocking_worker_parks_and_wakes_on_send)
{
    ufw::core::dispatch_matrix matrix{2, 64};
    atomic_actor target;
    condvar_backstop backstop;
    ufw::core::worker w1{1, ufw::core::loop_kind::blocking};

    auto const handle = ufw::core::make_enqueue_handle<&atomic_actor::on_add>(
        &target, matrix.cell(0, 1), w1.wakeable_or_null()); // blocking -> non-null
    ufw::core::column_drainer drainer{matrix.inbound(1), w1.stats()};
    w1.add_source(drainer);
    w1.set_backstop(backstop);
    w1.launch();

    handle(41);
    BOOST_TEST(await([&] { return target.calls.load(std::memory_order_relaxed) == 1; }));
    w1.request_stop(); // also wakes the park
    w1.join();

    BOOST_TEST(target.sum.load() == 41);
    BOOST_TEST(w1.stats().dispatched == 1U);
}

// The single-thread collapse: one worker, zero rings, run_inline on this thread —
// a direct handle plus a stop source show the loop runs sources and honors stop.
BOOST_AUTO_TEST_CASE(single_thread_collapse_runs_inline_with_zero_rings)
{
    struct one_shot final : ufw::core::poll_source
    {
        ufw::core::worker* w = nullptr;
        atomic_actor* target = nullptr;
        ufw::core::inbox_handle<void(int)> handle;
        bool fired = false;

        std::size_t poll() noexcept override
        {
            if (fired)
            {
                return 0;
            }
            fired = true;
            handle(7); // direct call — same worker
            w->request_stop();
            return 1;
        }
    };

    ufw::core::dispatch_matrix matrix{1, 64}; // never asked for a cell -> zero rings
    atomic_actor target;
    ufw::core::worker w0{0, ufw::core::loop_kind::spinning};
    one_shot src;
    src.w = &w0;
    src.target = &target;
    src.handle = ufw::core::make_direct_handle<&atomic_actor::on_add>(&target);
    w0.add_source(src);

    w0.run_inline(); // returns when the source requests stop

    BOOST_TEST(target.calls.load() == 1);
    BOOST_TEST(target.sum.load() == 7);
    BOOST_TEST(matrix.inbound(0).empty()); // the collapse built no rings
    BOOST_TEST(w0.stats().useful_iters >= 1U);
}

BOOST_AUTO_TEST_SUITE_END(/* ufw_core_worker_matrix */)

BOOST_AUTO_TEST_SUITE(ufw_core_thread_usage)

// The sampler contract: capture the handle ON the observed thread, sample it from
// ANOTHER thread (here: the test main thread plays the 1Hz sampler).
BOOST_AUTO_TEST_CASE(samples_another_threads_cpu_from_outside)
{
    std::atomic<ufw::core::thread_cpu_handle> handle{};
    std::atomic<bool> stop{false};
    std::thread burner([&handle, &stop]
    {
        handle.store(ufw::core::current_thread_cpu_handle());
        while (!stop.load(std::memory_order_relaxed)) { /* burn cpu */ }
    });
    while (handle.load().value == 0)
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto const cpu = ufw::core::sample_thread_cpu(handle.load());
    stop.store(true);
    burner.join();

    BOOST_TEST(cpu.total_ns > 20'000'000U); // burned a good chunk of the ~100ms window
    BOOST_TEST(ufw::core::sample_thread_cpu({}).total_ns == 0U); // null handle is inert
}

BOOST_AUTO_TEST_CASE(process_usage_is_populated)
{
    auto const usage = ufw::core::sample_process_usage();
    BOOST_TEST(usage.maxrss_bytes > 1'000'000U); // the test binary surely exceeds 1MB
    BOOST_TEST(usage.user_ns > 0U);
}

BOOST_AUTO_TEST_SUITE_END(/* ufw_core_thread_usage */)

} // namespace
