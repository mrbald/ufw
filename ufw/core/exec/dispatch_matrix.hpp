/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * The any-to-any dispatch fabric for N workers: cell [from][to] is the SPSC ring
 * carrying `from`'s commands TO worker `to`. The producer of a cell is exactly one
 * thread (worker `from`), the consumer exactly one (worker `to`) — each cell is
 * single-producer/single-consumer by CONSTRUCTION, which is why a plain spsc_ring
 * is the right primitive. Like a CPU scheduler with static assignment: entities are
 * pinned to workers, no dynamic migration (yet).
 *
 * Cells are created LAZILY at wiring time (resolve binds a handle to a cell): the
 * diagonal is never built (same-worker sends are direct calls), and a fully
 * single-threaded topology builds ZERO rings. The ring-matrix is ONE dispatcher
 * flavour; an MPSC-inbox flavour (and others) are future work that will abstract
 * the handle's enqueue side.
 */
#pragma once

#include "command.hpp"
#include "poll_source.hpp"
#include "worker.hpp"

#include <ufw/core/metrics/metrics.hpp>
#include <ufw/core/ring/spsc_ring.hpp>
#include <ufw/core/sys/timing.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace ufw::core {

class dispatch_matrix
{
public:
    // `ring_slots` per cell (rounded up to a power of two by spsc_ring).
    dispatch_matrix(unsigned n_workers, std::size_t ring_slots):
        n_{n_workers}, slots_{ring_slots},
        cells_(static_cast<std::size_t>(n_workers) * n_workers)
    {
    }

    // The from->to cell, created on first request. WIRING-PHASE ONLY: resolution
    // runs single-threaded before the workers launch, so the lazy creation is
    // deliberately unsynchronized.
    [[nodiscard]] spsc_ring<dispatch_cmd>& cell(unsigned from, unsigned to)
    {
        if (from == to)
        {
            throw std::logic_error("dispatch_matrix: no diagonal cell (same-worker sends are direct calls)");
        }
        if (from >= n_ || to >= n_)
        {
            throw std::out_of_range("dispatch_matrix: worker id out of range");
        }
        auto& slot = cells_[(static_cast<std::size_t>(from) * n_) + to];
        if (!slot)
        {
            slot = std::make_unique<spsc_ring<dispatch_cmd>>(slots_);
        }
        return *slot;
    }

    // All inbound cells for worker `to` ([*][to]) that exist NOW — build the
    // column drainer only after every handle has resolved (all cell() calls done).
    [[nodiscard]] std::vector<spsc_ring<dispatch_cmd>*> inbound(unsigned to) const
    {
        std::vector<spsc_ring<dispatch_cmd>*> cells;
        for (unsigned from = 0; from < n_; ++from)
        {
            auto const& slot = cells_[(static_cast<std::size_t>(from) * n_) + to];
            if (slot)
            {
                cells.push_back(slot.get());
            }
        }
        return cells;
    }

    [[nodiscard]] unsigned workers() const noexcept { return n_; }

private:
    unsigned    n_;
    std::size_t slots_;
    // n*n unique_ptrs, diagonal stays null; spsc_ring is immovable (owns a
    // memory_region), hence the indirection.
    std::vector<std::unique_ptr<spsc_ring<dispatch_cmd>>> cells_;
};

// Drains one worker's entire inbound column, executing each command's trampoline
// on the calling (= target's) thread. IS a poll_source — add it to the worker's
// source list. Batch drain: one cursor release per run (the ring README's
// drain-heavy pattern), one clock read per run (G6).
class column_drainer final : public poll_source
{
public:
    // `latency_series` is optional telemetry: per-message dispatch latency (ns)
    // recorded into the mmap gauge file; a null handle is an inert no-op.
    column_drainer(std::vector<spsc_ring<dispatch_cmd>*> inbound, worker_stats& stats,
                   metrics::series latency_series = {}) noexcept:
        inbound_{std::move(inbound)}, stats_{&stats}, latency_series_{latency_series}
    {
    }

    std::size_t poll() noexcept override
    {
        std::size_t did = 0;
        for (spsc_ring<dispatch_cmd>* ring : inbound_)
        {
            std::span<dispatch_cmd const> const run = ring->peek_batch();
            if (run.empty())
            {
                continue;
            }
            std::uint64_t const now = now_ticks();
            for (dispatch_cmd const& cmd : run)
            {
                cmd.tramp(cmd.obj, cmd);
                if (cmd.stamp != 0 && now > cmd.stamp) // guard against cross-core counter skew
                {
                    std::uint64_t const lat = now - cmd.stamp;
                    stat_add(stats_->dispatch_ticks_sum, lat);
                    stat_max(stats_->dispatch_ticks_max, lat);
                    latency_series_.record(ticks_to_ns(lat), now); // inert when telemetry is off
                }
            }
            ring->release(run.size());
            did += run.size();
        }
        stat_add(stats_->dispatched, did);
        return did;
    }

private:
    std::vector<spsc_ring<dispatch_cmd>*> inbound_;
    worker_stats* stats_; // the OWNING worker's stats (we run on its thread)
    metrics::series latency_series_;
};

} // namespace ufw::core
