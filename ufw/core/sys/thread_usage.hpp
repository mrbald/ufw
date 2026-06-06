/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 *
 * Cross-thread CPU usage sampling for the telemetry sampler: capture a cheap
 * handle ON the thread of interest once, then sample it from ANY thread (the 1Hz
 * sampler) thereafter. Platform notes:
 *   - macOS:  pthread_mach_thread_np port + thread_info(THREAD_BASIC_INFO) —
 *             user/system split available;
 *   - Linux:  pthread_getcpuclockid + clock_gettime — TOTAL cpu only (the
 *             user/system split needs /proc parsing; system_ns reads 0). NB
 *             getrusage(RUSAGE_THREAD) cannot be used: it samples only the
 *             CALLING thread.
 * Plus process-level getrusage(RUSAGE_SELF) (maxrss normalized to BYTES — Linux
 * reports KiB, macOS bytes). Header leaks no platform includes.
 */
#pragma once

#include <cstdint>

namespace ufw::core {

// Opaque per-thread CPU handle; valid for the thread's lifetime.
struct thread_cpu_handle
{
    std::uintptr_t value = 0;
};

struct thread_cpu_times
{
    std::uint64_t total_ns  = 0; // user + system
    std::uint64_t system_ns = 0; // 0 where the platform cannot split (Linux)
};

struct process_usage_sample
{
    std::uint64_t maxrss_bytes = 0;
    std::uint64_t user_ns      = 0;
    std::uint64_t system_ns    = 0;
};

// Capture on the thread being observed (e.g. at worker launch).
[[nodiscard]] thread_cpu_handle current_thread_cpu_handle() noexcept;

// Sample from anywhere; {0,0} if the handle is invalid/dead.
[[nodiscard]] thread_cpu_times sample_thread_cpu(thread_cpu_handle handle) noexcept;

// getrusage(RUSAGE_SELF), normalized.
[[nodiscard]] process_usage_sample sample_process_usage() noexcept;

} // namespace ufw::core
