/*
 * Copyright (c) 2015-2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 */

#pragma once

#include "logger.hpp"

#include <string>

namespace ufw {

using entity_id = std::string;
using resolved_entity_id = size_t;

// Sentinel for "not yet resolved" / "no such entity". resolve_entity_id()
// returns this on a miss; entity_ref starts here until resolve() succeeds.
inline constexpr resolved_entity_id unresolved_entity_id =
        static_cast<resolved_entity_id>(-1);

struct application;
struct lifecycle_participant;

#define ENTITY_LOGGER \
private:\
    mutable logger_t logger_ {::ufw::get_or_create_entity_logger(id())};\
public:\
    logger_t get_logger() const { return logger_; }

struct entity
{
    entity(entity_id const& id, resolved_entity_id rid, application& app):
            id_{id}, rid_{rid}, app_{app} {}
    virtual ~entity() = default;

    // Entities are identity objects: held by unique_ptr, never copied or moved.
    entity(entity const&) = delete;
    entity& operator=(entity const&) = delete;
    entity(entity&&) = delete;
    entity& operator=(entity&&) = delete;

    [[nodiscard]] entity_id const& id() const noexcept { return id_; }
    [[nodiscard]] resolved_entity_id resolved_id() const noexcept { return rid_; }

    [[nodiscard]] application& app() const noexcept { return app_; }

private:
    entity_id const id_;
    resolved_entity_id const rid_;
    application& app_;

    ENTITY_LOGGER;
    friend struct lifecycle_participant; // logger access
};

/**
 * Entity lazy references logic helper
 */
template <class T>
struct entity_ref
{
    T* operator->() const { return target_; }
    [[nodiscard]] T* get() const { return target_; }

    explicit operator T&() { return *target_; }
    explicit operator T const&() const { return *target_; }

    explicit operator bool() const { return target_; }

    entity_ref(entity_id const& id, application& app):
            id_ {id},
            app_ {app} {}

    [[nodiscard]] entity_id const& id() const { return id_; }
    [[nodiscard]] resolved_entity_id resolved_id() const { return resolved_id_; }

    void resolve();

private:
    entity_id const id_;
    application& app_;

    resolved_entity_id resolved_id_ {unresolved_entity_id};
    T* target_ {};
};

} // namespace ufw
