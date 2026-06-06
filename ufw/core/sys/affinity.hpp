/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 *
 * Thread placement — a reusable home so this stops being copy-pasted into
 * benchmark/test TUs. The platform guts live in affinity.cpp; this header leaks
 * no <pthread.h>/<sched.h>/<sys/qos.h>.
 */
#pragma once

namespace ufw::core {

// Best-effort placement of the CALLING thread. On Linux this is a HARD pin to
// logical CPU `core` (pthread_setaffinity_np) — deterministic. On macOS there is no
// per-core affinity API — Apple Silicon ignores THREAD_AFFINITY_POLICY entirely — so
// it biases the thread onto the performance (P) cores via QoS, keeping it off the
// efficiency (E) cores; `core` is ignored there. For truly deterministic per-core
// pinning, run on Linux.
void pin_thread(unsigned core) noexcept;

} // namespace ufw::core
