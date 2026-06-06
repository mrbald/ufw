/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * The dispatch-fabric pipe-rinsing sandbox: one ping/pong actor pair driven by the
 * shipped execution flavours, selected PURELY by YAML (same binary, same actors):
 *   examples/dispatch_blocking.yaml    one BLOCKING worker — the classic reactor;
 *   examples/dispatch_spinning.yaml    one SPINNING worker — the hot loop;
 *   examples/dispatch_two_worker.yaml  ping on worker 0, pong on worker 1 — every
 *                                      leg crosses an SPSC matrix cell.
 * The pinger counts ROUNDS round trips, shuts the app down, and logs the per-worker
 * utilization and dispatch-latency stats in stop(). These three runs are the
 * regression set for the executor flavours: optimizing one must not degrade another.
 */
#include "actor.hpp"
#include "application.hpp"
#include "entity.hpp"
#include "lifecycle_participant.hpp"
#include "plugin.hpp"

#include <ufw/core/sys/timing.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace {

constexpr std::uint64_t rounds = 50'000;

struct pinger;

struct ponger final : ufw::entity, ufw::lifecycle_participant
{
    template <class... Args>
    explicit ponger(Args&&... args):
        ufw::entity{std::forward<Args>(args)...}, reply_{"ping", *this} {}

    void on_ping(std::uint64_t seq) noexcept { reply_(seq); }

    void init() override; // bound out-of-line, after pinger is complete (mutual reference)

private:
    ufw::inbox_ref<void(std::uint64_t)> reply_;
};

struct pinger final : ufw::entity, ufw::lifecycle_participant
{
    template <class... Args>
    explicit pinger(Args&&... args):
        ufw::entity{std::forward<Args>(args)...}, ping_{"pong", *this} {}

    void on_pong(std::uint64_t seq) noexcept
    {
        if (seq >= rounds)
        {
            done_ticks_ = ufw::core::now_ticks();
            app().shutdown();
            return;
        }
        if (ping_.direct())
        {
            // Same-worker sends run inline: re-post the next round through the
            // io_context so a 50k-round loop does not recurse 100k frames deep.
            boost::asio::post(app().context(), [this, seq] { ping_(seq + 1); });
        }
        else
        {
            ping_(seq + 1); // cross-worker: the rings break the cycle
        }
    }

    void init() override { ping_.resolve<&ponger::on_ping>(); }

    void start() override
    {
        start_ticks_ = ufw::core::now_ticks();
        // The first ping flows once the workers run (the post lands on worker 0's
        // backstop poll) — the lifecycle itself stays message-free.
        boost::asio::post(app().context(), [this] { ping_(1); });
    }

    void stop() noexcept override
    {
        // Workers are stopped and joined by now: the single-writer stats are stable.
        auto const ns = ufw::core::ticks_to_ns(done_ticks_ - start_ticks_);
        LOG_INF("{} round trips ({}) in {} ms, {} ns/round",
                rounds, ping_.direct() ? "direct + post" : "cross-worker rings",
                ns / 1'000'000, ns / std::max<std::uint64_t>(rounds, 1));
        for (unsigned w = 0; w < app().worker_count(); ++w)
        {
            auto const& s = app().worker_at(w).stats();
            auto const iterations = s.iterations.load();
            auto const useful     = s.useful_iters.load();
            auto const dispatched = s.dispatched.load();
            LOG_INF("worker {}: iterations={} useful={} utilization={}% dispatched={} latency mean={} ns max={} ns",
                    w, iterations, useful,
                    iterations != 0 ? 100 * useful / iterations : 0,
                    dispatched,
                    dispatched != 0 ? ufw::core::ticks_to_ns(s.dispatch_ticks_sum.load()) / dispatched : 0,
                    ufw::core::ticks_to_ns(s.dispatch_ticks_max.load()));
        }
    }

private:
    ufw::inbox_ref<void(std::uint64_t)> ping_;
    std::uint64_t start_ticks_ = 0;
    std::uint64_t done_ticks_ = 0;
};

void ponger::init()
{
    reply_.resolve<&pinger::on_pong>();
}

UFW_PLUGIN(); // stamp this shared library with the uFW ABI version

UFW_PLUGIN_ENTITY_CTOR(pinger_ctor) { return new pinger{id, rid, app}; }
UFW_PLUGIN_ENTITY_CTOR(ponger_ctor) { return new ponger{id, rid, app}; }

} // local namespace
