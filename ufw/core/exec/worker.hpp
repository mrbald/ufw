/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * A worker: a pinned thread (or the calling thread — the single-thread collapse)
 * running one run-loop flavour over an ordered set of poll_sources plus an optional
 * blocking backstop. The flavours share the same drain and differ only in idle
 * policy (see poll_source.hpp). The worker IS a wakeable: enqueue handles bake
 * `wakeable_or_null()` in at resolve time, so a parked blocking worker is poked per
 * cross-worker send while spinners never branch.
 */
#pragma once

#include "poll_source.hpp"

#include <ufw/core/ring/sequencer.hpp> // cache_line (TODO: hoist to sys/ some day)
#include <ufw/core/sys/thread_usage.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace ufw::core {

enum class loop_kind : std::uint8_t { spinning, blocking };

// Inline telemetry. SINGLE-WRITER: only the worker's own thread (via its loop and
// its sources) may write — via stat_add/stat_max below, a relaxed load+store
// (never an RMW) that compiles to the same plain add as a non-atomic field while
// making the telemetry sampler's cross-thread reads well-defined.
struct alignas(cache_line) worker_stats
{
    std::atomic<std::uint64_t> iterations{0};         // loop turns
    std::atomic<std::uint64_t> useful_iters{0};       // turns where some source did work
    std::atomic<std::uint64_t> dispatched{0};         // commands executed on this worker
    std::atomic<std::uint64_t> dispatch_ticks_sum{0}; // enqueue->drain ticks (cross-worker sends)
    std::atomic<std::uint64_t> dispatch_ticks_max{0};
};

// Single-writer accumulators for stats fields (see worker_stats).
inline void stat_add(std::atomic<std::uint64_t>& stat, std::uint64_t n) noexcept
{
    stat.store(stat.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}
inline void stat_max(std::atomic<std::uint64_t>& stat, std::uint64_t v) noexcept
{
    if (v > stat.load(std::memory_order_relaxed))
    {
        stat.store(v, std::memory_order_relaxed);
    }
}

inline constexpr unsigned no_pin = ~0U; // "do not pin" sentinel for pin_core

class worker final : public wakeable
{
public:
    worker(unsigned id, loop_kind kind, unsigned pin_core = no_pin) noexcept:
        id_{id}, kind_{kind}, pin_core_{pin_core} {}
    ~worker() override; // safety net: request_stop() + join()

    worker(worker const&) = delete;
    worker& operator=(worker const&) = delete;
    worker(worker&&) = delete;
    worker& operator=(worker&&) = delete;

    // --- wiring (single-threaded setup phase, before launch()/run_inline()) ---
    void add_source(poll_source& source); // ordered: earlier = higher priority

    // Required for loop_kind::blocking. For a SPINNING worker, cadence_mask sets
    // how often the backstop is polled: 0 = every turn (right when the io is the
    // worker's only work source), 2^n-1 = every 2^n-th turn (right when rings are
    // the latency path — an empty io poll is a syscall the reply path must not
    // pay; an empty spin turn is ~ns, so io latency stays bounded and tiny).
    void set_backstop(blocking_source& backstop, std::uint64_t cadence_mask = 0);

    // --- lifecycle ---
    void run_inline();            // run the loop on the CALLING thread
    void launch();                // spawn a thread, pin it, run the loop
    void request_stop() noexcept; // stop flag + wake()
    void join();

    // --- wakeable ---
    void wake() noexcept override; // forwards to the backstop; spinners need none

    [[nodiscard]] unsigned  id() const noexcept { return id_; }
    [[nodiscard]] loop_kind kind() const noexcept { return kind_; }

    // For the SEND path, baked into enqueue handles at resolve time: a blocking
    // worker must be poked out of its park; a spinner re-polls unconditionally.
    [[nodiscard]] wakeable* wakeable_or_null() noexcept
    {
        return kind_ == loop_kind::blocking ? this : nullptr;
    }

    // Non-const overload hands the stats sink to this worker's own sources (e.g.
    // the column drainer) at wiring time — still single-writer (they run on this
    // worker's thread).
    [[nodiscard]] worker_stats&       stats() noexcept { return stats_; }
    [[nodiscard]] worker_stats const& stats() const noexcept { return stats_; }

    // The loop thread's CPU handle, captured at loop entry — {} until the loop
    // runs. For the telemetry sampler (sample_thread_cpu from its cold thread).
    [[nodiscard]] thread_cpu_handle cpu_handle() const noexcept
    {
        return {cpu_handle_.load(std::memory_order_relaxed)};
    }

private:
    void loop_spinning() noexcept;
    void loop_blocking() noexcept;

    unsigned const  id_;
    loop_kind const kind_;
    unsigned const  pin_core_;
    std::vector<poll_source*> sources_; // immutable once the loop runs
    blocking_source* backstop_ = nullptr;
    std::uint64_t backstop_cadence_mask_ = 0; // spinning only; 0 = poll every turn
    std::atomic<bool> stop_{false};
    std::atomic<std::uintptr_t> cpu_handle_{0}; // captured at loop entry
    std::thread thread_; // empty when run_inline()

    alignas(cache_line) worker_stats stats_{};
};

} // namespace ufw::core
