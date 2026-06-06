/*
 * Copyright (c) 2015-2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 */

#pragma once

#include "entity.hpp"  // up() does dynamic_cast<entity const*> + get_logger(): needs the complete type
#include "logger.hpp"

namespace ufw {

struct lifecycle_participant
{
    lifecycle_participant() = default;

    // Participants are non-copyable, non-movable interface objects.
    lifecycle_participant(lifecycle_participant const&) = delete;
    lifecycle_participant& operator=(lifecycle_participant const&) = delete;
    lifecycle_participant(lifecycle_participant&&) = delete;
    lifecycle_participant& operator=(lifecycle_participant&&) = delete;

    // at this stage participants may discover and cache references (including strongly typed) to each other
    virtual void init() {}

    // at this stage participants may establish connections, spawn threads, etc.
    virtual void start() {}

    // at this stage participants may start messaging each other
    virtual void up() const noexcept final
    {
        auto const* entity_ptr = dynamic_cast<entity const*>(this);
        if (entity_ptr)
        {
            auto const get_logger = [&]{ return entity_ptr->get_logger(); };
            LOG_INF("UP");
        }
    }

    // reverse of start()
    virtual void stop() noexcept {}

    // reverse of init()
    virtual void fini() noexcept {}

    virtual ~lifecycle_participant() = default;
};

} // namespace ufw
