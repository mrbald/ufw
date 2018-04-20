/* 
 * Copyright (c) 2015 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#pragma once

#include "entity.hpp"

#include <functional>
#include <memory>

namespace ufw {

struct loader: entity
{
    using entity::entity;
    virtual std::unique_ptr<entity> load(entity_id const& id, resolved_entity_id rid, config_t const& cfg) = 0;
};

using loader_func_t = std::function<std::unique_ptr<entity>(config_t const& cfg, entity_id const& id, resolved_entity_id rid, application& app)>;

} // namespace ufw
