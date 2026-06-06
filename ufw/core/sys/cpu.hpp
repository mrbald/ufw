/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 *
 * CPU spin-wait hint. Inline (it sits inside busy loops); leaks no platform
 * <header> — a compiler builtin / one asm line, same discipline as timing.hpp.
 */
#pragma once

namespace ufw::core {

// Polite busy-wait: de-prioritizes the spinning hyperthread / saves a little power
// without yielding the core (x86 PAUSE, arm64 YIELD; no-op elsewhere).
inline void cpu_relax() noexcept
{
#ifdef __x86_64__
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm64__)
    __asm__ __volatile__("yield");
#endif
}

} // namespace ufw::core
