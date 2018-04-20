/*
   Copyright 2017 Vladimir Lysyy (mrbald@github)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#pragma once

#include "exception_types.hpp"
#include "configuration.hpp"
#include "logger.hpp"
#include "entity.hpp"
#include "lifecycle_participant.hpp"
#include "loader.hpp"

#include <boost/program_options.hpp>
#include <boost/core/demangle.hpp>

#include <boost/asio/io_service.hpp>
#include <boost/asio/signal_set.hpp>

#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <map>
#include <type_traits>

namespace ufw {

struct application
{
    application();

    void register_loader(entity_id const& id, loader_func_t loader_func);

    template <class T, class Cfg>
    void register_loader(entity_id const& id)
    {
        register_loader(id, [](config_t const& cfg, entity_id const& id, resolved_entity_id rid, application& app)
        {
            return std::make_unique<T>(cfg.as<Cfg>(), id, rid, app);
        });
    }

    // entity public c-tor signature must be `T(..., entity_id const&, resolved_entity_id, application&)`
    template <class T, class... Args>
    resolved_entity_id add(entity_id const& id, Args&&... args)
    {
        static_assert(std::is_base_of<entity, T>::value, "");

        if (structure_locked_)
            throw fatal_error("cannot add entity - application structure already locked, likely a bug in the code");

        resolved_entity_id const rid = entities_.size();

        if (!entity_ids_.emplace(id, rid).second)
            throw fatal_error("duplicate entity ID, check configuration");

        entities_.push_back(std::make_unique<T>(std::forward<Args>(args)..., id, rid, *this));

        LOG_INF << "loaded with ctor: " << id << "<" << rid << ">";
        return rid;
    }

    // loader `loader_id` should be pre-registered for loader ID specified in the config
    resolved_entity_id add(entity_id const& id, entity_id const& loader_id, config_t const& cfg);

    resolved_entity_id resolve_entity_id(entity_id const& id) const;

    template <class T>
    T& get(resolved_entity_id rid) const
    {
        // TODO: VL: write generic exception type translator template
        try
        {
            return dynamic_cast<T&>(*entities_.at(rid));
        }
        catch (std::bad_cast const&)
        {
            throw fatal_error(entities_.at(rid)->id() + "<" + std::to_string(rid) + "> is not " + boost::core::demangle(typeid(T).name()));
        }
        catch (std::out_of_range const&)
        {
            throw fatal_error("no entity with ID " + std::to_string(rid));
        }
    }

    template <class T>
    T& get(entity_id const& id) const
    {
        auto rid = resolve_entity_id(id);
        if (rid >= entities_.size())
            throw fatal_error("no entity with ID " + id);
        return get<T>(rid);
    }

    template <class T, class F>
    void for_each(F const& f)
    {
        for (auto& base_ptr: entities_)
        {
            auto* casted_ptr = dynamic_cast<T*>(base_ptr.get());
            if (casted_ptr)
                f(*casted_ptr);
        }
    }

    entity& get(resolved_entity_id rid) const;

    void load(int argc, char const** argv);

    void run();

    void shutdown();

    boost::asio::io_service& context() { return context_; }
private:
    entity_id id() const { return "app"; } // for ENTITY_LOGGER macro to work

private:
    void load(application_config const& cfg);

    boost::asio::io_service context_;
    boost::asio::signal_set terminal_signals_ {context_, SIGINT/*, SIGTERM*/};
    std::unique_ptr<boost::asio::io_service::work> work_;

    std::vector<std::unique_ptr<entity>> entities_;
    std::map<entity_id, size_t> entity_ids_;

    std::vector<std::reference_wrapper<lifecycle_participant>> lifecycle_participants_;

    ENTITY_LOGGER;

    bool structure_locked_ {false};
};


template <class T>
void entity_ref<T>::resolve()
{
    resolved_id_ = app_.resolve_entity_id(id_);
    if (resolved_id_ == resolved_entity_id(-1))
        throw fatal_error("no entity with ID " + id_);
    target_ = &app_.get<T>(resolved_id_);
}

} // namespace ufw

