/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 */
#include <ufw/core/sys/timing.hpp>

#include <chrono>

namespace ufw::core {
namespace {

// ticks-per-nanosecond, calibrated once on first use (thread-safe magic static).
double ticks_per_ns() noexcept
{
    static double const ratio = []() noexcept -> double
    {
#if defined(__aarch64__) || defined(__arm64__)
        std::uint64_t freq = 0;
        __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
        return static_cast<double>(freq) / 1e9;    // counter Hz -> ticks per ns
#elifdef __x86_64__
        // Calibrate the TSC against steady_clock over ~1ms (one-time cost on the
        // first ticks_to_ns call; arm64 and the fallback never pay it).
        using clock = std::chrono::steady_clock;
        auto const c0 = clock::now();
        std::uint64_t const t0 = now_ticks();
        while (clock::now() - c0 < std::chrono::milliseconds(1)) { /* calibrating */ }
        auto const c1 = clock::now();
        std::uint64_t const t1 = now_ticks();
        double const ns = std::chrono::duration<double, std::nano>(c1 - c0).count();
        return static_cast<double>(t1 - t0) / ns;  // TSC ticks per ns
#else
        return 1.0;                                 // now_ticks() already returns ns
#endif
    }();
    return ratio;
}

} // namespace

std::uint64_t ticks_to_ns(std::uint64_t ticks) noexcept
{
    return static_cast<std::uint64_t>(static_cast<double>(ticks) / ticks_per_ns());
}

} // namespace ufw::core
