/* 
 * Copyright (c) 2015 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#pragma once

#include "logger.hpp"

namespace ufw {

struct entity;

struct lifecycle_participant
{
    // at this stage participants may discover and cache references (including strongly typed) to each other
    virtual void init() {}

    // at this stage participants may establish connections, spawn threads, etc.
    virtual void start() {}

    // at this stage participants may start messaging each other
    virtual void up() const noexcept final
    {
        auto* entity_ptr = dynamic_cast<entity const*>(this);
        if (entity_ptr)
        {
            auto const get_logger = [&]()->logger_t& { return entity_ptr->get_logger(); };
            LOG_INF << "UP";
        }
    }

    // reverse of start()
    virtual void stop() noexcept {}

    // reverse of init()
    virtual void fini() noexcept {}

    virtual ~lifecycle_participant() = default;
};

} // namespace ufw
