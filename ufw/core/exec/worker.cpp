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
    // Safety net for a launched-but-never-joined worker only. When the loop was
    // already stopped and joined (the normal path), do NOTHING — in particular do
    // not wake() a backstop that may be destructing alongside us.
    if (thread_.joinable())
    {
        request_stop();
        join();
    }
}

void worker::add_source(poll_source& source)
{
    sources_.push_back(&source);
}

void worker::set_backstop(blocking_source& backstop, std::uint64_t cadence_mask)
{
    backstop_ = &backstop;
    backstop_cadence_mask_ = cadence_mask;
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

// Hot: drain every source each turn; PAUSE/YIELD when nothing did work. The
// backstop (if any) is polled on its wiring-time cadence (see set_backstop) —
// an "idle" turn is exactly when a reply is in flight, so the io poll must never
// sit on the reply path of a ring-fed worker.
void worker::loop_spinning() noexcept
{
    while (!stop_.load(std::memory_order_relaxed))
    {
        std::size_t did = 0;
        for (poll_source* source : sources_)
        {
            did += source->poll();
        }
        if (backstop_ != nullptr
            && (stats_.iterations.load(std::memory_order_relaxed) & backstop_cadence_mask_) == 0)
        {
            did += backstop_->poll(); // bounded: at most one handler per turn
        }
        stat_add(stats_.iterations, 1);
        if (did != 0)
        {
            stat_add(stats_.useful_iters, 1);
        }
        else
        {
            cpu_relax();
        }
    }
}

// Cool: drain every source once; if nothing did work, park in the backstop until
// IO arrives or a remote producer (or request_stop) wake()s us. Work the park ran
// counts as useful (it IS the blocking flavour's work).
void worker::loop_blocking() noexcept
{
    while (!stop_.load(std::memory_order_relaxed))
    {
        std::size_t did = 0;
        for (poll_source* source : sources_)
        {
            did += source->poll();
        }
        if (did == 0)
        {
            did += backstop_->poll_blocking();
        }
        stat_add(stats_.iterations, 1);
        if (did != 0)
        {
            stat_add(stats_.useful_iters, 1);
        }
    }
}

} // namespace ufw::core
