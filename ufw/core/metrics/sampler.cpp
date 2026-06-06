/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 */
#include <ufw/core/metrics/sampler.hpp>

#include <utility>

namespace ufw::core::metrics {

sampler::~sampler()
{
    stop();
}

void sampler::add_task(std::function<void()> task)
{
    tasks_.push_back(std::move(task));
}

void sampler::start()
{
    thread_ = std::thread([this] { run(); });
}

void sampler::stop()
{
    {
        std::scoped_lock const lock{mutex_};
        stop_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void sampler::run() noexcept
{
    for (;;)
    {
        for (auto const& task : tasks_)
        {
            task();
        }
        std::unique_lock lock{mutex_};
        if (cv_.wait_for(lock, interval_, [this] { return stop_; }))
        {
            break;
        }
    }
    // One final flush ON THIS THREAD (the gauges' single writer), so runs shorter
    // than an interval — and post-mortem reads in general — see the end state.
    for (auto const& task : tasks_)
    {
        task();
    }
}

} // namespace ufw::core::metrics
