/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 *
 * The run-loop seam. The polymorphic thing in the executor is NOT the loop — it is
 * the poll_source: one bounded, NON-BLOCKING unit of pollable work. A worker is an
 * ordered set of poll_sources plus an optional blocking backstop, and the loop
 * flavours differ only in idle policy:
 *   spinning  — drain every source each turn, cpu_relax() when none did work;
 *   blocking  — drain every source once, then park in the backstop (e.g. asio
 *               run_one) until IO arrives or a remote producer wake()s us;
 *   busy-poll — (future) a NIC poller is just the highest-priority source.
 * Adding a flavour never touches this seam, the command, the handle, or the matrix.
 */
#pragma once

#include <cstddef>

namespace ufw::core {

// One bounded, non-blocking unit of pollable work. Returns how many useful items
// were processed (0 = found nothing); the count feeds per-worker utilization
// telemetry and the blocking worker's "may I park?" decision. The virtual call is
// per loop TURN per source (1-3 sources), not per message — the work inside poll()
// (a fully inlined ring drain) dominates; CRTP-hoist only if a profile says so.
struct poll_source
{
    virtual std::size_t poll() noexcept = 0;

    poll_source() = default;
    virtual ~poll_source() = default;
    poll_source(poll_source const&) = delete;
    poll_source& operator=(poll_source const&) = delete;
    poll_source(poll_source&&) = delete;
    poll_source& operator=(poll_source&&) = delete;
};

// Something a REMOTE producer pokes to break a parked worker out of its backstop.
// Pure spinners expose no wakeable (they re-poll unconditionally), so the send
// path's `if (notify) notify->wake()` is a never-taken branch — deliberately wired
// from day one so the future blocking-as-matrix-target lands by implementing
// wake(), not by re-plumbing every call site.
struct wakeable
{
    virtual void wake() noexcept = 0;

    wakeable() = default;
    virtual ~wakeable() = default;
    wakeable(wakeable const&) = delete;
    wakeable& operator=(wakeable const&) = delete;
    wakeable(wakeable&&) = delete;
    wakeable& operator=(wakeable&&) = delete;
};

// A source that can additionally PARK the calling thread until work or a wake()
// arrives — the blocking worker's backstop (e.g. an asio io_context run_one). It
// MUST be wakeable: a parked thread has to be interruptible, if only for clean
// shutdown (request_stop -> wake). A separate sub-interface of poll_source because
// a ring cannot block. Returns how many useful items the park ran (so work done
// inside the backstop is visible to utilization telemetry).
struct blocking_source : poll_source, wakeable
{
    virtual std::size_t poll_blocking() noexcept = 0;
};

} // namespace ufw::core
