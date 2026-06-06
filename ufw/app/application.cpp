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
    wire_workers();   // after init: every inbox has resolved, so all matrix cells exist
    wire_telemetry(); // after wire_workers: the workers exist to be mirrored
    install_signal_handler();
    schedule_up();
    start_participants();
    if (sampler_)
    {
        sampler_->start();
    }

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
    if (sampler_)
    {
        sampler_->stop(); // one final tick already happened; stats are now stable
    }

    // init/start ran in declaration order; stop/fini run in reverse.
    std::ranges::reverse(lifecycle_participants_);
    stop_participants();
    fini_participants();
}

// Sampler tasks — the WARM telemetry tier: mirror each worker's stats (cumulative
// counters; utilization is the per-interval delta) and its thread CPU, plus the
// process-level rusage. The hot tier (the drainers' latency series) was wired in
// wire_workers; the cold tier (collection/aggregation/charting) lives out of
// process behind the mmap file.
void application::wire_telemetry()
{
    if (!metrics_ || !sampler_)
    {
        return;
    }
    for (auto& worker_ptr : workers_)
    {
        auto* worker = worker_ptr.get();
        auto const label = std::to_string(worker->id());
        auto const iterations  = metrics_->make_counter("ufw_worker_iterations_total{worker=\"" + label + "\"}");
        auto const useful      = metrics_->make_counter("ufw_worker_useful_iterations_total{worker=\"" + label + "\"}");
        auto const dispatched  = metrics_->make_counter("ufw_worker_dispatched_total{worker=\"" + label + "\"}");
        auto const cpu_total   = metrics_->make_counter("ufw_worker_cpu_ns_total{worker=\"" + label + "\"}");
        auto const cpu_system  = metrics_->make_counter("ufw_worker_cpu_system_ns_total{worker=\"" + label + "\"}");
        auto const utilization = metrics_->make_value_f64("ufw_worker_utilization_pct{worker=\"" + label + "\"}");
        sampler_->add_task(
            [worker, iterations, useful, dispatched, cpu_total, cpu_system, utilization,
             last_iterations = std::uint64_t{0}, last_useful = std::uint64_t{0}]() mutable
            {
                auto const it = worker->stats().iterations.load(std::memory_order_relaxed);
                auto const us = worker->stats().useful_iters.load(std::memory_order_relaxed);
                iterations.set(it);
                useful.set(us);
                dispatched.set(worker->stats().dispatched.load(std::memory_order_relaxed));
                auto cpu = core::sample_thread_cpu(worker->cpu_handle());
                if (cpu.total_ns == 0)
                {
                    cpu = worker->final_cpu(); // thread gone: its terminal self-sample
                }
                if (cpu.total_ns != 0) // not yet launched: keep the last good sample
                {
                    cpu_total.set(cpu.total_ns);
                    cpu_system.set(cpu.system_ns);
                }
                auto const delta_iterations = it - last_iterations;
                auto const delta_useful     = us - last_useful;
                utilization.set(delta_iterations == 0
                                    ? 0.0
                                    : 100.0 * static_cast<double>(delta_useful)
                                          / static_cast<double>(delta_iterations));
                last_iterations = it;
                last_useful     = us;
            });
    }

    auto const maxrss     = metrics_->make_value_i64("ufw_process_maxrss_bytes");
    auto const cpu_user   = metrics_->make_counter("ufw_process_cpu_user_ns_total");
    auto const cpu_system = metrics_->make_counter("ufw_process_cpu_system_ns_total");
    sampler_->add_task([maxrss, cpu_user, cpu_system]
    {
        auto const usage = core::sample_process_usage();
        maxrss.set(static_cast<std::int64_t>(usage.maxrss_bytes));
        cpu_user.set(usage.user_ns);
        cpu_system.set(usage.system_ns);
    });
}

// Wire each worker its drainer — the matrix's [*][me] inbound column or its single
// MPSC inbox, per the configured flavour — and a backstop: worker 0 always gets
// the io_context backstop so signals, timers and posts stay serviced whichever
// flavour it runs; blocking workers require one.
void application::wire_workers()
{
    if (workers_.empty())
    {
        return;
    }
    LOG_INF("wiring {} workers ({} dispatch)", workers_.size(), mpsc_dispatch_ ? "mpsc" : "matrix");
    auto const latency_series = [this](unsigned id)
    {
        return metrics_ == nullptr
            ? core::metrics::series{}
            : metrics_->make_series(R"(ufw_dispatch_latency_ns{worker=")"
                                    + std::to_string(id) + R"("})");
    };
    for (auto& worker : workers_)
    {
        bool ring_fed = false;
        if (mpsc_dispatch_)
        {
            ring_fed = workers_.size() > 1; // any peer may send; a lone worker is all-direct
            if (ring_fed)
            {
                drainers_.push_back(std::make_unique<core::mpsc_drainer>(
                    *inboxes_[worker->id()], worker->stats(), latency_series(worker->id())));
                worker->add_source(*drainers_.back());
            }
        }
        else
        {
            auto inbound = matrix_->inbound(worker->id());
            ring_fed = !inbound.empty();
            if (ring_fed)
            {
                drainers_.push_back(std::make_unique<core::column_drainer>(
                    std::move(inbound), worker->stats(), latency_series(worker->id())));
                worker->add_source(*drainers_.back());
            }
        }
        if (worker->id() == 0 || worker->kind() == core::loop_kind::blocking)
        {
            // Ring-fed spinners poll the io on a cadence (an empty io poll is a
            // syscall the reply path must not pay); io-only workers poll it every
            // turn (it is their sole work source). Blocking workers park in it.
            worker->set_backstop(*backstop_, ring_fed ? 255 : 0);
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

void application::build_worker_pool(application_config const& cfg)
{
    auto const& workers = cfg.workers;
    for (std::size_t i = 0; i < workers.size(); ++i)
    {
        auto const& worker_cfg = workers[i];
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
    if (cfg.dispatch == "matrix")
    {
        matrix_ = std::make_unique<core::dispatch_matrix>(
            static_cast<unsigned>(workers_.size()), dispatch_ring_slots);
    }
    else if (cfg.dispatch == "mpsc")
    {
        mpsc_dispatch_ = true;
        inboxes_.reserve(workers_.size());
        for (std::size_t i = 0; i < workers_.size(); ++i)
        {
            inboxes_.push_back(
                std::make_unique<core::mpsc_ring<core::dispatch_cmd>>(dispatch_ring_slots));
        }
    }
    else
    {
        throw fatal_error("dispatch must be 'matrix' or 'mpsc', got: " + cfg.dispatch);
    }
    backstop_ = std::make_unique<asio_backstop>(context_);
    LOG_INF("worker pool: {} workers, {} dispatch", workers_.size(), cfg.dispatch);
}

void application::load(application_config const& cfg)
{
    if (!cfg.workers.empty())
    {
        build_worker_pool(cfg);
    }
    if (!cfg.telemetry.file.empty())
    {
        metrics_ = std::make_unique<core::metrics::registry>(
            core::metrics::registry::options{.file = cfg.telemetry.file.c_str()});
        sampler_ = std::make_unique<core::metrics::sampler>(
            std::chrono::milliseconds{cfg.telemetry.interval_ms});
        LOG_INF("telemetry: {} sampled every {}ms", cfg.telemetry.file, cfg.telemetry.interval_ms);
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
