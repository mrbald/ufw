/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * inbox_ref — the entity_ref<T> sibling for MESSAGING. Where entity_ref resolves
 * to a typed pointer, inbox_ref<&T::method> resolves to a core::inbox_handle: a
 * tiny curried value that either calls the target's method directly (target owned
 * by the SAME worker — including the whole-app-on-one-thread collapse, where every
 * send is direct and zero rings exist) or packs the args into the caller->target
 * cell of the dispatch matrix for the target's worker to drain.
 *
 * Usage, mirroring entity_ref:
 *     struct trader : entity, lifecycle_participant {
 *         inbox_ref<&book::on_quote> book_inbox;
 *         trader(...): entity{...}, book_inbox{"book", *this} {}
 *         void init() override { book_inbox.resolve(); }     // freeze direct-vs-enqueue
 *         void on_tick(tick t) { book_inbox(t.px, t.qty); }  // fire-and-forget send
 *     };
 */
#pragma once

#include "application.hpp"
#include "entity.hpp"

#include <ufw/core/exec/inbox.hpp>

#include <string>
#include <type_traits>
#include <utility>

namespace ufw {

template <auto Method>
struct inbox_ref
{
    using target_t = core::method_owner_t<Method>;

    inbox_ref(entity_id id, entity& self): id_{std::move(id)}, self_{&self} {}

    // init()-phase: look the target up (the entity_ref lookup), compare worker
    // assignments, and freeze the handle as direct or enqueue. The structure is
    // locked by now, so the pointer, the cell, and the decision stay valid for the
    // run.
    void resolve()
    {
        static_assert(std::is_base_of_v<entity, target_t>, "inbox targets are entities");
        application& app = self_->app();
        target_t& target = app.get<target_t>(id_);
        unsigned const mine   = app.worker_of(self_->resolved_id());
        unsigned const theirs = app.worker_of(target.resolved_id());
        if (mine == theirs)
        {
            handle_ = core::make_direct_handle<Method>(&target);
        }
        else
        {
            handle_ = core::make_enqueue_handle<Method>(
                &target,
                app.matrix().cell(mine, theirs),
                app.worker_at(theirs).wakeable_or_null());
        }
    }

    // The send. Fire-and-forget; loss-free (cross-worker back-pressure spins).
    template <class... CallArgs>
    void operator()(CallArgs&&... args) const noexcept
    {
        handle_(std::forward<CallArgs>(args)...);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(handle_); }
    [[nodiscard]] entity_id const& id() const noexcept { return id_; }

private:
    entity_id id_;
    entity* self_;
    core::inbox_handle_for<Method> handle_{};
};

} // namespace ufw
