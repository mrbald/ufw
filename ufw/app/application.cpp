#include "application.hpp"

#include "exception_types.hpp"
#include "configuration.hpp"
#include "logger.hpp"

#include <boost/core/demangle.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include <iostream>
#include <fstream>
#include <map>
#include <type_traits>

namespace ufw {

struct default_loader: loader
{
    using loader::loader;

    std::unique_ptr<entity> load(entity_id const& id, resolved_entity_id rid, config_t const& cfg) override
    {
        return load(id, id, rid, cfg);
    }

    std::unique_ptr<entity> load(entity_id const& lid, entity_id const& eid, resolved_entity_id rid, config_t const& cfg)
    {
        try
        {
            return loader_funcs_.at(lid)(cfg, eid, rid, app());
        }
        catch (std::out_of_range const&)
        {
            throw fatal_error("no default loader " + lid + " registered for entity " + eid);
        }
    }

    void register_loader(entity_id const& id, loader_func_t loader_func)
    {
        if (!loader_funcs_.emplace(id, std::move(loader_func)).second)
        {
            throw fatal_error("duplicate loader registration for enity ID " + id);
        }
        LOG_INF("registered loader function for {}", id);
    }

private:
    std::map<entity_id, loader_func_t> loader_funcs_;
};


application::application()
{
    add<default_loader>("");

    register_loader("LOGGER", [&](config_t const& cfg, entity_id const& id, resolved_entity_id rid, application& app)
    {
        configure_logger(cfg.as<logger_config>());
        return std::make_unique<entity>(id, rid, app);
    });
}

// NOLINTNEXTLINE(readability-make-member-function-const) — mutates app config via the default_loader; const would mislead
void application::register_loader(entity_id const& id, loader_func_t loader_func)
{
    if (structure_locked_)
    {
        throw fatal_error("cannot register loader - application structure already locked, likely a bug in the code");
    }

    get<default_loader>("").register_loader(id, std::move(loader_func));
}

// loader `loader_id` should be pre-registered for loader ID specified in the config
resolved_entity_id application::add(entity_id const& id, entity_id const& loader_id, config_t const& cfg)
{
    if (structure_locked_)
    {
        throw fatal_error("cannot load entity - application structure already locked, likely a bug in the code");
    }

    resolved_entity_id const rid = entities_.size();
    if (!entity_ids_.emplace(id, rid).second)
    {
        throw fatal_error("duplicate entity ID, check configuration");
    }

    auto loader_rid = resolve_entity_id(loader_id);
    if (loader_rid < entities_.size())
    {
        entities_.push_back(get<loader>(loader_rid).load(id, rid, cfg));
        LOG_INF("loaded with [{}<{}>]: {}<{}>", loader_id, loader_rid, id, rid);
    }
    else
    {
        entities_.push_back(get<default_loader>("").load(loader_id, id, rid, cfg));
        LOG_INF("loaded with loader function [{}]: {}<{}>", loader_id, id, rid);
    }

    return rid;
}

resolved_entity_id application::resolve_entity_id(entity_id const& id) const
{
    auto it = entity_ids_.find(id);
    return it == entity_ids_.end() ? unresolved_entity_id : it->second;
}

entity& application::get(resolved_entity_id rid) const
{
    return get<entity>(rid);
}

void application::load(int argc, char const** argv)
{
    // Hand-rolled CLI for the two flags we support, so no compiled boost component
    // (program_options) is needed — the rest of our boost use (asio, exception) is
    // header-only. Mirrors the previous behaviour: -c/--config <file> (default
    // config.yaml), -h/--help prints usage and bails.
    std::string config_file = "config.yaml";

    static constexpr std::string_view config_eq{"--config="};
    for (int i = 1; i < argc; ++i)
    {
        std::string_view const arg{argv[i]};
        if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: " << (argc > 0 ? argv[0] : "ufw_launcher")
                      << " [-c|--config <file>] [-h|--help]\n"
                         "  -c, --config <file>  application config file (default: config.yaml)\n"
                         "  -h, --help           print this help message\n";
            throw fatal_error("help displayed, bye");
        }
        if (arg == "-c" || arg == "--config")
        {
            if (++i >= argc)
            {
                throw fatal_error("missing value for option " + std::string{arg});
            }
            config_file = argv[i];
        }
        else if (arg.starts_with(config_eq))
        {
            config_file = std::string{arg.substr(config_eq.size())};
        }
        else
        {
            throw fatal_error("unknown argument: " + std::string{arg});
        }
    }

    LOG_INF("loading configuration from {}", config_file);
    std::ifstream in(config_file.c_str());
    if (!in)
    {
        throw std::runtime_error("config file not found");
    }
    YAML::Node node = YAML::Load(in);

    load(node["application"].as<application_config>());
}

void application::run()
{
    init_participants();
    wire_workers(); // after init: every inbox has resolved, so all matrix cells exist
    install_signal_handler();
    schedule_up();
    start_participants();

    work_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        context_.get_executor());
    if (workers_.empty())
    {
        context_.run(); // zero-config path: byte-for-byte the classic behaviour
    }
    else
    {
        for (std::size_t i = 1; i < workers_.size(); ++i)
        {
            workers_[i]->launch();
        }
        workers_[0]->run_inline(); // the main thread IS worker 0 (it also services the io_context)
        for (std::size_t i = 1; i < workers_.size(); ++i)
        {
            workers_[i]->request_stop();
            workers_[i]->join();
        }
    }

    // init/start ran in declaration order; stop/fini run in reverse.
    std::ranges::reverse(lifecycle_participants_);
    stop_participants();
    fini_participants();
}

// Wire each worker its column drainer (the [*][me] inbound cells of the matrix)
// and a backstop: worker 0 always gets the io_context backstop so signals, timers
// and posts stay serviced whichever flavour it runs; blocking workers require one.
void application::wire_workers()
{
    if (workers_.empty())
    {
        return;
    }
    LOG_INF("wiring {} workers", workers_.size());
    for (auto& worker : workers_)
    {
        drainers_.push_back(std::make_unique<core::column_drainer>(
            matrix_->inbound(worker->id()), worker->stats()));
        worker->add_source(*drainers_.back());
        if (worker->id() == 0 || worker->kind() == core::loop_kind::blocking)
        {
            worker->set_backstop(*backstop_);
        }
    }
}

void application::init_participants()
{
    LOG_INF("initializing lifecycle participants");
    for (lifecycle_participant& x: lifecycle_participants_)
    {
        LOG_INF("initializing {}", dynamic_cast<entity&>(x).id());
        x.init();
    }
}

void application::install_signal_handler()
{
    terminal_signals_.async_wait([this](boost::system::error_code const& error, int signal_number)
    {
        if (!error)
        {
            std::map<int, char const*> names {{SIGINT, "SIGINT"}, {SIGTERM, "SIGTERM"}};
            LOG_WRN("terminal signal {} received, terminating main context", names[signal_number]);
            shutdown();
        }
    });
}

void application::schedule_up()
{
    LOG_INF("scheduling lifecycle participants ping");
    for (lifecycle_participant& x: lifecycle_participants_)
    {
        boost::asio::post(context_, [&x]{ x.up(); }); // TODO: VL: ping participants via their inboxes (once inboxes are implemented)
    }
    boost::asio::post(context_, [this]{ LOG_INF("UP"); });
}

void application::start_participants()
{
    LOG_INF("starting lifecycle participants");
    for (lifecycle_participant& x: lifecycle_participants_)
    {
        LOG_INF("starting {}", dynamic_cast<entity&>(x).id());
        x.start();
    }
}

void application::stop_participants()
{
    LOG_INF("stopping lifecycle participants");
    for (lifecycle_participant& x: lifecycle_participants_)
    {
        LOG_INF("stopping {}", dynamic_cast<entity&>(x).id());
        x.stop();
    }
}

void application::fini_participants()
{
    LOG_INF("deinitializing lifecycle participants");
    for (lifecycle_participant& x: lifecycle_participants_)
    {
        LOG_INF("deinitializing {}", dynamic_cast<entity&>(x).id());
        x.fini();
    }
}

void application::shutdown()
{
    work_ = nullptr; // destroy the work guard so io_context::run() can return
    context_.stop();
    for (auto& worker : workers_)
    {
        worker->request_stop(); // unparks blocking workers; spinners notice the flag
    }
}

namespace
{
// Per-cell capacity of the dispatch matrix rings. One value for now; the depth /
// slot-size sweeps in the dispatch benchmarks decide if it earns a config knob.
constexpr std::size_t dispatch_ring_slots = 4096;
} // namespace

void application::load(application_config const& cfg)
{
    if (!cfg.workers.empty())
    {
        for (std::size_t i = 0; i < cfg.workers.size(); ++i)
        {
            auto const& worker_cfg = cfg.workers[i];
            if (worker_cfg.id != i)
            {
                throw fatal_error("workers: ids must be dense and ordered 0..N-1");
            }
            core::loop_kind kind{};
            if (worker_cfg.loop == "spinning")
            {
                kind = core::loop_kind::spinning;
            }
            else if (worker_cfg.loop == "blocking")
            {
                kind = core::loop_kind::blocking;
            }
            else
            {
                throw fatal_error("worker loop must be 'spinning' or 'blocking', got: " + worker_cfg.loop);
            }
            workers_.push_back(std::make_unique<core::worker>(worker_cfg.id, kind, worker_cfg.pin_core));
        }
        matrix_ = std::make_unique<core::dispatch_matrix>(
            static_cast<unsigned>(workers_.size()), dispatch_ring_slots);
        backstop_ = std::make_unique<asio_backstop>(context_);
        LOG_INF("worker pool: {} workers", workers_.size());
    }

    for (auto const& entity_cfg: cfg.entities)
    {
        auto const rid = add(entity_cfg.name, entity_cfg.loader_ref, entity_cfg.config);
        if (entity_cfg.worker != 0)
        {
            if (entity_cfg.worker >= workers_.size())
            {
                throw fatal_error("entity '" + entity_cfg.name + "' assigned to worker "
                                  + std::to_string(entity_cfg.worker)
                                  + (workers_.empty()
                                         ? std::string{" but no workers: block is configured"}
                                         : " but only " + std::to_string(workers_.size())
                                               + " workers are configured"));
            }
            entity_workers_.emplace(rid, entity_cfg.worker);
        }
    }

    entities_.shrink_to_fit();

    for_each<lifecycle_participant>([&](lifecycle_participant& lp)
    {
        lifecycle_participants_.push_back(std::ref(lp));
    });

    lifecycle_participants_.shrink_to_fit();

    structure_locked_ = true;
}


} // namespace ufw
