/*
   Copyright 2017-2023 Vladimir Lysyy (mrbald@github)

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
#include "asio_backstop.hpp"

#include <ufw/core/exec/dispatch_matrix.hpp>
#include <ufw/core/exec/worker.hpp>
#include <ufw/core/metrics/metrics.hpp>
#include <ufw/core/metrics/sampler.hpp>

#include <boost/core/demangle.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>

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
        static_assert(std::is_base_of_v<entity, T>);

        if (structure_locked_)
        {
            throw fatal_error("cannot add entity - application structure already locked, likely a bug in the code");
        }

        resolved_entity_id const rid = entities_.size();

        if (!entity_ids_.emplace(id, rid).second)
        {
            throw fatal_error("duplicate entity ID, check configuration");
        }

        entities_.push_back(std::make_unique<T>(std::forward<Args>(args)..., id, rid, *this));

        LOG_INF("loaded with ctor: {}<{}>", id, rid);
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
        {
            throw fatal_error("no entity with ID " + id);
        }
        return get<T>(rid);
    }

    template <class T, class F>
    void for_each(F const& f)
    {
        for (auto& base_ptr: entities_)
        {
            auto* casted_ptr = dynamic_cast<T*>(base_ptr.get());
            if (casted_ptr)
            {
                f(*casted_ptr);
            }
        }
    }

    entity& get(resolved_entity_id rid) const;

    void load(int argc, char const** argv);

    // Assemble entities from an in-memory config, discover lifecycle
    // participants, and lock the structure. The file-based load() above decodes
    // YAML into this; the Python control plane builds it from a dict.
    void load(application_config const& cfg);

    void run();

    void shutdown();

    boost::asio::io_context& context() { return context_; }

    // --- the dispatch fabric (opt-in via the `workers:` config block) ---
    // Without the block none of this exists: worker_of() answers 0 for everyone,
    // every inbox_ref resolves to a DIRECT handle, zero rings are built, and run()
    // is byte-for-byte the classic single-threaded context_.run() path.
    [[nodiscard]] bool has_worker_pool() const noexcept { return !workers_.empty(); }
    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }
    [[nodiscard]] unsigned worker_of(resolved_entity_id rid) const noexcept
    {
        auto const it = entity_workers_.find(rid);
        return it == entity_workers_.end() ? 0U : it->second;
    }
    [[nodiscard]] core::dispatch_matrix& matrix()
    {
        if (!matrix_)
        {
            throw fatal_error("no worker pool configured (workers: block absent)");
        }
        return *matrix_;
    }
    [[nodiscard]] core::worker& worker_at(unsigned idx)
    {
        if (idx >= workers_.size())
        {
            throw fatal_error("worker id out of range: " + std::to_string(idx));
        }
        return *workers_[idx];
    }

    // The mmap gauge registry (opt-in via the `telemetry:` config block) — null
    // when telemetry is not configured. Entities may register their own gauges in
    // init() (registration is init-phase single-threaded, like all resolution).
    [[nodiscard]] core::metrics::registry* metrics() noexcept { return metrics_.get(); }
private:
    static entity_id id() { return "app"; } // for ENTITY_LOGGER macro to work

    // run() phases, factored out of the lifecycle driver (init/start forward,
    // stop/fini in reverse — the reversal happens once in run()).
    void init_participants();
    void install_signal_handler();
    void schedule_up();
    void start_participants();
    void stop_participants();
    void fini_participants();

    void build_worker_pool(std::vector<worker_config> const& workers); // load(): workers + matrix
    void wire_workers();   // post-init: column drainers + backstops onto the workers
    void wire_telemetry(); // post-init: sampler tasks (worker mirrors + process rusage)

    boost::asio::io_context context_;
    boost::asio::signal_set terminal_signals_ {context_, SIGINT/*, SIGTERM*/};
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_;

    std::vector<std::unique_ptr<entity>> entities_;
    std::map<entity_id, size_t> entity_ids_;

    std::vector<std::reference_wrapper<lifecycle_participant>> lifecycle_participants_;

    // --- the opt-in dispatch fabric + telemetry ---
    // Declaration order is load-bearing (destruction is the reverse): the sampler
    // goes LAST (destructs first — its tasks read worker stats and gauges), the
    // workers next (their dtors and still-running loops touch drainers/backstop),
    // and the registry early (drainers hold series handles into its mapping).
    std::map<resolved_entity_id, unsigned> entity_workers_;
    std::unique_ptr<core::dispatch_matrix> matrix_;
    std::unique_ptr<asio_backstop> backstop_;
    std::unique_ptr<core::metrics::registry> metrics_;
    std::vector<std::unique_ptr<core::column_drainer>> drainers_;
    std::vector<std::unique_ptr<core::worker>> workers_;
    std::unique_ptr<core::metrics::sampler> sampler_; // absent rid => worker 0

    ENTITY_LOGGER;

    bool structure_locked_ {false};
};


template <class T>
void entity_ref<T>::resolve()
{
    resolved_id_ = app_.resolve_entity_id(id_);
    if (resolved_id_ == unresolved_entity_id)
    {
        throw fatal_error("no entity with ID " + id_);
    }
    target_ = &app_.get<T>(resolved_id_);
}

} // namespace ufw

