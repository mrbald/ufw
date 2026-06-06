/* 
 * Copyright (c) 2015-2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 */

#pragma once

#include "entity.hpp"
#include "configuration.hpp"

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
