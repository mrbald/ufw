/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 *
 * inbox_ref — the entity_ref<T> sibling for MESSAGING. Where entity_ref resolves
 * to a typed pointer, an inbox_ref resolves to a core::inbox_handle: a tiny curried
 * value that either calls the target's method directly (target owned by the SAME
 * worker — including the whole-app-on-one-thread collapse, where every send is
 * direct and zero rings exist) or packs the args into the caller->target cell of
 * the dispatch matrix for the target's worker to drain.
 *
 * The member is typed by the MESSAGE SIGNATURE, not the target: mutually-messaging
 * actors (ping <-> pong, the normal case) could never both name each other's
 * complete type at member declaration. The target METHOD binds at
 * resolve<&T::method>() in init(), where every type is complete:
 *
 *     struct pinger : entity, lifecycle_participant {
 *         inbox_ref<void(std::uint64_t)> ping{"pong", *this};
 *         ...
 *         void init() override { ping.resolve<&ponger::on_ping>(); }
 *         void on_pong(std::uint64_t seq) { ping(seq + 1); }  // fire-and-forget
 *     };
 *
 * Re-entrancy note: a DIRECT send runs the target's handler synchronously on the
 * calling stack. A same-worker send loop (A->B->A->...) therefore RECURSES — break
 * such cycles by re-posting through the io_context (see direct()), or assign the
 * actors to different workers, where the rings break the cycle naturally.
 */
#pragma once

#include "application.hpp"
#include "entity.hpp"

#include <ufw/core/exec/inbox.hpp>

#include <string>
#include <type_traits>
#include <utility>

namespace ufw {

template <class Sig>
struct inbox_ref;

template <class... Args>
struct inbox_ref<void(Args...)>
{
    inbox_ref(entity_id id, entity& self): id_{std::move(id)}, self_{&self} {}

    // init()-phase: look the target up (the entity_ref lookup), compare worker
    // assignments, and freeze the handle as direct or enqueue. The structure is
    // locked by now, so the pointer, the cell, and the decision stay valid for
    // the run.
    template <auto Method>
    void resolve()
    {
        using target_t = core::method_owner_t<Method>;
        static_assert(std::is_base_of_v<entity, target_t>, "inbox targets are entities");
        static_assert(std::is_same_v<core::inbox_handle_for<Method>, core::inbox_handle<void(Args...)>>,
                      "the target method's signature does not match this inbox_ref");
        application& app = self_->app();
        target_t& target = app.get<target_t>(id_);
        unsigned const mine   = app.worker_of(self_->resolved_id());
        unsigned const theirs = app.worker_of(target.resolved_id());
        // port_for owns the topology AND the dispatcher flavour (matrix cell vs
        // mpsc inbox vs direct); resolve never names one.
        handle_ = core::make_handle<Method>(&target, app.port_for(mine, theirs));
    }

    // The send. Fire-and-forget; loss-free (cross-worker back-pressure spins).
    void operator()(Args... args) const noexcept { handle_(args...); }

    // True when the resolved handle is a same-worker direct call (a send loop
    // through it would recurse — see the re-entrancy note above).
    [[nodiscard]] bool direct() const noexcept { return handle_.direct(); }
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(handle_); }
    [[nodiscard]] entity_id const& id() const noexcept { return id_; }

private:
    entity_id id_;
    entity* self_;
    core::inbox_handle<void(Args...)> handle_{};
};

} // namespace ufw
