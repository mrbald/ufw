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
#include "exception_types.hpp"

#include <ufw/app/version.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <map>

namespace ufw {

struct library_repository final: loader {
    using loader::loader;

    std::unique_ptr<entity> load(entity_id const& id, resolved_entity_id rid, config_t const& cfg) override
    {
        auto const filename = cfg["filename"].as<std::string>();
        auto lib = library::load(filename.c_str());

        // ABI gate: a plugin is only valid in the exact uFW version it was built
        // against. Require every dlopen'd library to carry the UFW_PLUGIN() stamp
        // and match this launcher; refuse anything else. This is the one generic
        // chokepoint for separately-compiled code entering the process — it
        // covers loaders too, since a loader is just an entity from a library.
        char const* plugin_version = nullptr;
        try
        {
            plugin_version = lib->function<char const*()>("ufw_abi_version")();
        }
        catch (std::runtime_error const&)
        {
            throw fatal_error("'" + filename + "' is not a uFW plugin: missing ABI stamp (declare UFW_PLUGIN())");
        }
        if (std::string_view{plugin_version} != UFW_VERSION)
        {
            throw fatal_error("ABI version mismatch loading '" + filename + "': built against uFW "
                              + plugin_version + ", launcher is " UFW_VERSION);
        }

        return std::make_unique<library_entity>(std::move(lib), id, rid, app());
    }
};

} // namespace ufw
