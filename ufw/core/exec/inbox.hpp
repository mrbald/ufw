/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 *
 * The RESOLVED inbox handle: `inbox(args...)` as a tiny trivially-copyable value
 * (no std::function, no heap, no vtable on the hot path) that either makes a
 * direct method call (target lives on the CALLING worker) or packs the args into a
 * dispatch_cmd and pushes it through an ENQUEUE PORT for the target's worker to
 * drain. The port abstracts the dispatcher flavour — an SPSC matrix cell or the
 * target's MPSC inbox — and is decided ONCE, at resolve time, by whoever
 * constructs the handle (the app layer knows the topology and the flavour). The
 * per-call kind branch is stable per handle: the predictor owns it.
 *
 * Entity-agnostic on purpose: a handle binds any object + member function whose
 * args are trivially copyable, so the core can be exercised by plain structs in
 * tests and benchmarks; the entity/`inbox_ref` glue lives in ufw_app.
 */
#pragma once

#include "command.hpp"
#include "poll_source.hpp"

#include <ufw/core/ring/mpsc_ring.hpp>
#include <ufw/core/ring/spsc_ring.hpp>
#include <ufw/core/sys/cpu.hpp>
#include <ufw/core/sys/timing.hpp>

#include <cstdint>
#include <type_traits>

namespace ufw::core {

// Where a handle's cross-worker sends go. Built by whoever knows the topology and
// the dispatcher flavour (the app layer's port_for); consumed by inbox_handle.
enum class port_kind : std::uint8_t
{
    direct,     // same worker: no ring, the call runs synchronously
    spsc_cell,  // the caller->target cell of the SPSC dispatch matrix
    mpsc_inbox, // the target worker's single MPSC inbox
};

struct enqueue_port
{
    port_kind kind   = port_kind::direct;
    void*     ring   = nullptr; // spsc_ring<dispatch_cmd>* | mpsc_ring<dispatch_cmd>*
    wakeable* notify = nullptr; // target worker's wakeable (null for spinners)
};
static_assert(std::is_trivially_copyable_v<enqueue_port>);

// The bound non-virtual delegate for `Method` (= &T::m, taking exactly Args...):
// casts obj back to T and forwards the unpacked args. Bound at resolve time —
// the same moment entity_ref<T>::resolve() caches its typed pointer.
template <class T, auto Method, class... Args>
void trampoline_for(void* obj, dispatch_cmd const& cmd) noexcept
{
    apply_from_buffer<Args...>(cmd.args, [obj](Args... args) noexcept
    {
        (static_cast<T*>(obj)->*Method)(args...);
    });
}

template <class Sig>
class inbox_handle;

template <class... Args>
class inbox_handle<void(Args...)>
{
    static_assert((std::is_trivially_copyable_v<Args> && ...),
                  "inbox args must be trivially copyable (ring slot constraint)");

public:
    inbox_handle() = default; // null until resolved

    inbox_handle(void* obj, trampoline_t tramp, enqueue_port port) noexcept:
        obj_{obj}, tramp_{tramp}, port_{port} {}

    // The hot path. Same worker: one erased call, no stamp (there is no queueing
    // latency to measure). Cross-worker: pack + stamp + spin-push (loss-free
    // back-pressure; the policy seam is per-edge, spin is the default) + an
    // optional wake of a parked target (never-taken branch for spinners).
    void operator()(Args... args) const noexcept
    {
        dispatch_cmd cmd{.tramp = tramp_, .obj = obj_, .stamp = 0, .args = {}};
        pack_args(cmd.args, args...);
        if (port_.kind == port_kind::direct)
        {
            tramp_(obj_, cmd);            // direct: runs NOW, on the target's worker
            return;
        }
        cmd.stamp = now_ticks();          // dispatch-latency telemetry (cross-worker only)
        if (port_.kind == port_kind::spsc_cell)
        {
            auto* ring = static_cast<spsc_ring<dispatch_cmd>*>(port_.ring);
            while (!ring->try_push(cmd))
            {
                cpu_relax();
            }
        }
        else
        {
            auto* ring = static_cast<mpsc_ring<dispatch_cmd>*>(port_.ring);
            while (!ring->try_push(cmd))
            {
                cpu_relax();
            }
        }
        if (port_.notify != nullptr)
        {
            port_.notify->wake();
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept { return obj_ != nullptr; }

    // True for a resolved SAME-WORKER handle (calls run synchronously on the
    // caller's stack); false for cross-worker enqueue or an unresolved handle.
    [[nodiscard]] bool direct() const noexcept
    {
        return obj_ != nullptr && port_.kind == port_kind::direct;
    }

private:
    void*        obj_   = nullptr;
    trampoline_t tramp_ = nullptr;
    enqueue_port port_{}; // kind == direct => same-worker synchronous call
};

namespace detail {

// Deduce the owner type + arg list from a member-function pointer, so factories
// are spelled make_*_handle<&T::method>(...) with nothing repeated.
template <class M>
struct method_sig;

template <class T, class... Args>
struct method_sig<void (T::*)(Args...)>
{
    using owner_t  = T;
    using handle_t = inbox_handle<void(Args...)>;

    template <auto Method>
    [[nodiscard]] static consteval trampoline_t tramp() noexcept
    {
        return &trampoline_for<T, Method, Args...>;
    }
};

template <class T, class... Args>
struct method_sig<void (T::*)(Args...) noexcept> : method_sig<void (T::*)(Args...)> {};

} // namespace detail

// Public deduction aliases (the app-level inbox_ref builds on these; detail:: stays private).
template <auto Method>
using method_owner_t = detail::method_sig<decltype(Method)>::owner_t;

template <auto Method>
using inbox_handle_for = detail::method_sig<decltype(Method)>::handle_t;

// The general factory: the port decides direct vs which-flavour-enqueue.
template <auto Method>
[[nodiscard]] auto make_handle(
    typename detail::method_sig<decltype(Method)>::owner_t* target, enqueue_port port) noexcept
{
    using sig = detail::method_sig<decltype(Method)>;
    return typename sig::handle_t{target, sig::template tramp<Method>(), port};
}

// Same-worker handle: every call is a direct method call on the caller's thread.
template <auto Method>
[[nodiscard]] auto make_direct_handle(
    typename detail::method_sig<decltype(Method)>::owner_t* target) noexcept
{
    return make_handle<Method>(target, enqueue_port{});
}

// Cross-worker handle into an SPSC matrix cell (the caller->target edge);
// `notify` is the target worker's wakeable, or null for a spinning target.
template <auto Method>
[[nodiscard]] auto make_enqueue_handle(
    typename detail::method_sig<decltype(Method)>::owner_t* target,
    spsc_ring<dispatch_cmd>& out, wakeable* notify) noexcept
{
    return make_handle<Method>(
        target, enqueue_port{.kind = port_kind::spsc_cell, .ring = &out, .notify = notify});
}

// Cross-worker handle into the target worker's single MPSC inbox.
template <auto Method>
[[nodiscard]] auto make_mpsc_handle(
    typename detail::method_sig<decltype(Method)>::owner_t* target,
    mpsc_ring<dispatch_cmd>& inbox, wakeable* notify) noexcept
{
    return make_handle<Method>(
        target, enqueue_port{.kind = port_kind::mpsc_inbox, .ring = &inbox, .notify = notify});
}

} // namespace ufw::core
