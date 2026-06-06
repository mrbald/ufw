/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 */
#include <ufw/core/exec/worker.hpp>

#include <ufw/core/sys/affinity.hpp>
#include <ufw/core/sys/cpu.hpp>

#include <stdexcept>

namespace ufw::core {

worker::~worker()
{
    request_stop();
    join();
}

void worker::add_source(poll_source& source)
{
    sources_.push_back(&source);
}

void worker::set_backstop(blocking_source& backstop)
{
    backstop_ = &backstop;
}

void worker::run_inline()
{
    if (kind_ == loop_kind::blocking && backstop_ == nullptr)
    {
        throw std::logic_error("worker: a blocking loop requires a backstop");
    }
    if (pin_core_ != no_pin)
    {
        pin_thread(pin_core_);
    }
    if (kind_ == loop_kind::spinning)
    {
        loop_spinning();
    }
    else
    {
        loop_blocking();
    }
}

void worker::launch()
{
    if (kind_ == loop_kind::blocking && backstop_ == nullptr)
    {
        throw std::logic_error("worker: a blocking loop requires a backstop"); // before the thread exists
    }
    thread_ = std::thread([this] { run_inline(); });
}

void worker::request_stop() noexcept
{
    stop_.store(true, std::memory_order_relaxed);
    wake();
}

void worker::join()
{
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void worker::wake() noexcept
{
    if (backstop_ != nullptr)
    {
        backstop_->wake(); // breaks poll_blocking() out of its park
    }
    // spinners: nothing to do — they re-poll unconditionally
}

// Hot: drain every source each turn; PAUSE/YIELD when nothing did work.
void worker::loop_spinning() noexcept
{
    while (!stop_.load(std::memory_order_relaxed))
    {
        std::size_t did = 0;
        for (poll_source* source : sources_)
        {
            did += source->poll();
        }
        if (backstop_ != nullptr)
        {
            did += backstop_->poll(); // non-blocking poll of the backstop, if any
        }
        ++stats_.iterations;
        if (did != 0)
        {
            ++stats_.useful_iters;
        }
        else
        {
            cpu_relax();
        }
    }
}

// Cool: drain every source once; if nothing did work, park in the backstop until
// IO arrives or a remote producer (or request_stop) wake()s us.
void worker::loop_blocking() noexcept
{
    while (!stop_.load(std::memory_order_relaxed))
    {
        std::size_t did = 0;
        for (poll_source* source : sources_)
        {
            did += source->poll();
        }
        ++stats_.iterations;
        if (did != 0)
        {
            ++stats_.useful_iters;
            continue; // work pending: re-drain before considering a park
        }
        backstop_->poll_blocking();
    }
}

} // namespace ufw::core
