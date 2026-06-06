/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Compiler annotation seam. Grows ONLY when an annotation has a mechanically-known
 * or measured effect — the benchmarks are the referee. House policy:
 *
 *   - UFW_COLD marks failure paths REACHABLE FROM HOT CODE; the pattern is a
 *     hoisted [[noreturn]] UFW_COLD throw-helper. Three real effects: every
 *     call-site branch becomes statically unlikely for free (GCC/Clang treat paths
 *     ending in cold/noreturn calls as cold), the throw machinery stops counting
 *     against the caller's inlining budget, and the bytes land in .text.unlikely,
 *     out of the hot working set.
 *   - NO [[unlikely]] carpet on error branches: a call to a cold/noreturn helper
 *     already implies it, stable branches belong to the dynamic predictor (the
 *     attributes affect layout, not the hardware), and "error-shaped" guards are
 *     often the COMMON case (an idle drainer's run.empty() is true ~99% of turns).
 *     Reserve explicit [[likely]]/[[unlikely]] for rare + inline + unhoistable
 *     branches in measured-hot code.
 *   - NO UFW_HOT until a profile shows text-layout pressure: the hot path here is
 *     header templates inlined into the worker loops (no symbol left to mark), the
 *     loops are hot by residency, and GCC ignores `hot` under profile feedback.
 *   - Lifecycle (init/start/stop/fini) code gets NOTHING: attributes on virtuals
 *     do not propagate to overriders, and cold code called only from cold code has
 *     no hot neighbour to protect.
 */
#pragma once

#ifdef __GNUC__ // GCC and Clang
#  define UFW_COLD     __attribute__((cold))
#  define UFW_NOINLINE __attribute__((noinline))
#else
#  define UFW_COLD
#  define UFW_NOINLINE
#endif
