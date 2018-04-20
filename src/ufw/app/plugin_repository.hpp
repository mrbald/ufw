/* 
 * Copyright (c) 2015 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * 2015-03-28 - Vladimir Lysyy - Initial version
 */

#pragma once

#include "library_repository.hpp"
#include "library.hpp"
#include "loader.hpp"

#include <memory>
#include <string>
#include <map>

namespace ufw {

struct plugin_repository final: loader {
    using loader::loader;

    std::unique_ptr<entity> load(entity_id const& id, resolved_entity_id rid, config_t const& cfg) override
    {
        auto const library_ref = cfg["library_ref"].as<std::string>();
        auto const constructor = cfg["constructor"].as<std::string>();

        entity_ref<library_entity> lib {library_ref, app()};
        lib.resolve();

        return std::unique_ptr<entity>{ lib->function<entity*(entity_id const&, resolved_entity_id, application&)>(constructor)(id, rid, app()) };
    }
};

} // namespace ufw
