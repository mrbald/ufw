/* 
 * Copyright (c) 2015 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#pragma once

#include "logger.hpp"

#include <string>

namespace ufw {

using entity_id = std::string;
using resolved_entity_id = size_t;

struct application;
struct lifecycle_participant;

#define ENTITY_LOGGER \
private:\
    mutable logger_t logger_ {[&]\
    {\
        logger_t logger;\
        logger.add_attribute("Entity", attrs::constant<entity_id>(id()));\
        return logger;\
    }()};\
public:\
    logger_t& get_logger() const { return logger_; }

struct entity
{
    entity(entity_id const& id, resolved_entity_id rid, application& app):
            id_{id}, rid_{rid}, app_{app} {}
    virtual ~entity() = default;

    entity_id const& id() const noexcept { return id_; }
    resolved_entity_id resolved_id() const noexcept { return rid_; }

    application& app() const noexcept { return app_; }

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
    T* get() const { return target_; }

    explicit operator T&() { return *target_; }
    explicit operator T const&() const { return *target_; }

    explicit operator bool() const { return target_; }

    entity_ref(entity_id const& id, application& app):
            id_ {id},
            app_ {app} {}

    entity_id const& id() { return id_; }
    resolved_entity_id resolved_id() const { return resolved_id_; }

    void resolve();

private:
    entity_id const id_;
    application& app_;

    resolved_entity_id resolved_id_;
    T* target_ {};
};

} // namespace ufw
