/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 *
 * The WARM telemetry tier: one cold thread ticking at a fixed interval (~1Hz),
 * running registered sampling tasks — the things that need a syscall (rusage) or a
 * cross-thread snapshot (worker stats -> utilization). Everything hot writes its
 * gauges inline; everything colder (collection, aggregation, history, charting)
 * lives OUT of process behind the mmap file (the LGTM-stack side).
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ufw::core::metrics {

class sampler
{
public:
    explicit sampler(std::chrono::milliseconds interval) noexcept: interval_{interval} {}
    ~sampler();

    sampler(sampler const&) = delete;
    sampler& operator=(sampler const&) = delete;
    sampler(sampler&&) = delete;
    sampler& operator=(sampler&&) = delete;

    // Wiring phase (before start()): tasks run on the sampler thread, every tick,
    // in registration order. Keep them cheap and non-blocking-ish; they share one
    // cold thread.
    void add_task(std::function<void()> task);

    void start(); // spawn the thread; runs all tasks once immediately, then per tick
    void stop();  // signal + join; idempotent

private:
    void run() noexcept;

    std::chrono::milliseconds const interval_;
    std::vector<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace ufw::core::metrics
