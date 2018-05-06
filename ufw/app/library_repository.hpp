/* 
 * Copyright (c) 2015 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * 2015-03-28 - Vladimir Lysyy - Initial version
 * 2018-04-20 - Vladimir Lysyy - Adapted for uFW
 */

#pragma once

#include "library.hpp"
#include "loader.hpp"
#include "configuration.hpp"

#include <memory>
#include <string>
#include <map>

namespace ufw {

struct library_repository final: loader {
    using loader::loader;

    std::unique_ptr<entity> load(entity_id const& id, resolved_entity_id rid, config_t const& cfg) override
    {
        auto const filename = cfg["filename"].as<std::string>();
        return std::make_unique<library_entity>(library::load(filename.c_str()), id, rid, app());
    }
};

} // namespace ufw
