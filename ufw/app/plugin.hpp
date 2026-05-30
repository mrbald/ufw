/*
 * Copyright (c) 2015-2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * uFW plugin ABI contract.
 *
 * A plugin is a shared library that exposes uFW entities across a dlopen
 * boundary (a loader is an entity too, so this covers loaders). That boundary
 * ferries C++ types and polymorphic objects, so a plugin is only valid in the
 * exact uFW version it was compiled against. Every plugin stamps itself with
 * UFW_PLUGIN(); the LIBRARY loader refuses to dlopen anything unstamped or
 * version-mismatched (see library_repository.hpp). In-binary entities loaded by
 * the static default_loader cross no ABI boundary and need no stamp.
 */
#pragma once

#include "entity.hpp"

#include <ufw/app/version.hpp>

namespace ufw {

// Canonical entity-constructor signature. The PLUGIN loader resolves this exact
// type from the plugin's named `extern "C"` symbol; declare yours with
// UFW_PLUGIN_ENTITY_CTOR so the loader and the plugin can never drift apart.
using entity_ctor_t = entity*(entity_id const&, resolved_entity_id, application&);

} // namespace ufw

// Mark a symbol exported even under -fvisibility=hidden, so dlsym can find it.
#define UFW_EXPORT __attribute__((visibility("default")))

// Drop exactly once per plugin shared library. Bakes in the uFW version the
// plugin was compiled against, as a symbol the LIBRARY loader checks at dlopen.
#define UFW_PLUGIN() \
    extern "C" UFW_EXPORT char const* ufw_abi_version() { return UFW_VERSION; }

// Declare an entity constructor with the canonical signature, e.g.:
//   UFW_PLUGIN_ENTITY_CTOR(my_ctor) { return new my_entity{id, rid, app}; }
#define UFW_PLUGIN_ENTITY_CTOR(name) \
    extern "C" UFW_EXPORT ::ufw::entity* \
    name(::ufw::entity_id const& id, ::ufw::resolved_entity_id rid, ::ufw::application& app)
