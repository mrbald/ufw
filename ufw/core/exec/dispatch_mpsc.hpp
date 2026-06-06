/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 *
 * The MPSC-inbox dispatcher flavour: ONE Vyukov ring per worker, every other
 * worker pushes into it (mpsc_ring.hpp has the trade-off discussion vs the SPSC
 * matrix). The drainer mirrors column_drainer's telemetry exactly so the two
 * flavours are benchmark-comparable like-for-like: per-message dispatch latency
 * from the command's enqueue stamp into worker stats and the optional mmap series.
 */
#pragma once

#include "command.hpp"
#include "poll_source.hpp"
#include "worker.hpp"

#include <ufw/core/metrics/metrics.hpp>
#include <ufw/core/ring/mpsc_ring.hpp>
#include <ufw/core/sys/timing.hpp>

#include <cstddef>
#include <cstdint>

namespace ufw::core {

// A poll_source draining ONE worker's MPSC inbox: run every published command's
// trampoline (this runs on the OWNING worker's thread), stamp dispatch latency.
class mpsc_drainer final : public poll_source
{
public:
    // `latency_series` is optional telemetry (null handle = inert), as in
    // column_drainer.
    mpsc_drainer(mpsc_ring<dispatch_cmd>& inbox, worker_stats& stats,
                 metrics::series latency_series = {}) noexcept:
        inbox_{&inbox}, stats_{&stats}, latency_series_{latency_series}
    {
    }

    std::size_t poll() noexcept override
    {
        if (!inbox_->ready())
        {
            return 0; // idle turn: one acquire load, no clock read
        }
        std::uint64_t const now = now_ticks();
        std::size_t const did = inbox_->drain(
            [this, now](dispatch_cmd const& cmd)
            {
                cmd.tramp(cmd.obj, cmd);
                if (cmd.stamp != 0 && now > cmd.stamp) // guard against cross-core counter skew
                {
                    std::uint64_t const lat = now - cmd.stamp;
                    stat_add(stats_->dispatch_ticks_sum, lat);
                    stat_max(stats_->dispatch_ticks_max, lat);
                    latency_series_.record(ticks_to_ns(lat), now);
                }
            },
            inbox_->capacity()); // bounded by one full lap per turn
        stat_add(stats_->dispatched, did);
        return did;
    }

private:
    mpsc_ring<dispatch_cmd>* inbox_;
    worker_stats* stats_; // the OWNING worker's stats (we run on its thread)
    metrics::series latency_series_;
};

} // namespace ufw::core
