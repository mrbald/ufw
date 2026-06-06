/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 *
 * Monotonic, cheap tick source for inline telemetry. now_ticks() is INLINE (a single
 * counter read) because it is stamped on the dispatch hot path; only the nanosecond
 * calibration is out-of-line (timing.cpp). The header leaks no platform <header> —
 * just a compiler builtin / one asm line.
 */
#pragma once

#include <cstdint>

#if !defined(__x86_64__) && !defined(__aarch64__) && !defined(__arm64__)
#  include <chrono> // steady_clock fallback only
#endif

namespace ufw::core {

// Raw monotonic counter, cheapest available: x86-64 TSC (rdtsc), arm64 CNTVCT_EL0,
// else steady_clock nanoseconds. Measures DURATIONS (deltas); successive reads are
// non-decreasing but not a unique sequence. ~a few cycles on x86/arm64.
[[nodiscard]] inline std::uint64_t now_ticks() noexcept
{
#ifdef __x86_64__
    return __builtin_ia32_rdtsc();
#elif defined(__aarch64__) || defined(__arm64__)
    std::uint64_t t = 0;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(t));
    return t;
#else
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

// Convert a tick DURATION to nanoseconds via a ratio calibrated once on first use
// (arm64: CNTFRQ_EL0; x86: TSC vs steady_clock over ~1ms; fallback: identity). NB on
// Apple Silicon CNTVCT runs ~24MHz, so the resolution is ~41ns — fine for relative
// dispatch latency, coarse for sub-ns.
[[nodiscard]] std::uint64_t ticks_to_ns(std::uint64_t ticks) noexcept;

} // namespace ufw::core
