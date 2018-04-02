#include "application.hpp"

#include "exception_types.hpp"
#include "configuration.hpp"
#include "logger.hpp"

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
        catch (std::out_of_range)
        {
            throw fatal_error("no default loader " + lid + " registered for entity " + eid);
        }
    }

    void register_loader(entity_id const& id, loader_func_t loader_func)
    {
        if (!loader_funcs_.emplace(id, std::move(loader_func)).second)
            throw fatal_error("duplicate loader registration for enity ID " + id);
        LOG_INF << "registered loader function for " << id;
    }

private:
    std::map<entity_id, loader_func_t> loader_funcs_;
};


application::application()
{
    add<default_loader>("");

    register_loader("LOGGER", [&](config_t const& cfg, entity_id const& id, resolved_entity_id rid, application& app)
    {
        configure_logger(cfg.Scalar());
        return std::make_unique<entity>(id, rid, app);
    });
}

void application::register_loader(entity_id const& id, loader_func_t loader_func)
{
    if (structure_locked_)
        throw fatal_error("cannot register loader - application structure already locked, likely a bug in the code");

    get<default_loader>("").register_loader(id, std::move(loader_func));
}

// loader `loader_id` should be pre-registered for loader ID specified in the config
resolved_entity_id application::add(entity_id const& id, entity_id const& loader_id, config_t const& cfg)
{
    if (structure_locked_)
        throw fatal_error("cannot load entity - application structure already locked, likely a bug in the code");

    resolved_entity_id const rid = entities_.size();
    if (!entity_ids_.emplace(id, rid).second)
        throw fatal_error("duplicate entity ID, check configuration");

    auto loader_rid = resolve_entity_id(loader_id);
    if (loader_rid < entities_.size())
    {
        entities_.push_back(get<loader>(loader_rid).load(id, rid, cfg));
        LOG_INF << "loaded with [" << loader_id << "<" << loader_rid << ">]: " << id << "<" << rid << ">";
    }
    else
    {
        entities_.push_back(get<default_loader>("").load(loader_id, id, rid, cfg));
        LOG_INF << "loaded with loader function [" << loader_id << "]: " << id << "<" << rid << ">";
    }

    return rid;
}

resolved_entity_id application::resolve_entity_id(entity_id const& id) const
{
    auto it = entity_ids_.find(id);
    return it == entity_ids_.end() ? -1 : it->second;
}

entity& application::get(resolved_entity_id rid) const
{
    return get<entity>(rid);
}

void application::load(int argc, char const** argv)
{
    namespace po = boost::program_options;

    std::string config_file = "config.yaml";
    po::options_description desc {"Options"};
    desc.add_options()
        ("help,h", "Print this help message")
        ("config,c", po::value<std::string>(&config_file)->default_value(config_file), "application config file")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);

    if (vm.count("help"))
    {
        std::cout << desc << std::endl;
        throw fatal_error("help displayed, bye");
    }

    po::notify(vm);

    LOG_INF << "loading configuration from " << config_file;
    std::ifstream in(config_file.c_str());
    if (!in) throw std::runtime_error("config file not found");
    YAML::Node node = YAML::Load(in);

    load(node["application"].as<application_config>());
}

void application::run()
{
    LOG_STAMP_THREAD;

    LOG_INF << "initializing lifecycle participants";
    for (lifecycle_participant& x: lifecycle_participants_)
    {
        LOG_INF << "initializing " << dynamic_cast<entity&>(x).id();
        x.init();
    }

    terminal_signals_.async_wait([this](boost::system::error_code const& error, int signal_number)
    {
        if (!error)
        {
            std::map<int, char const*> names {{SIGINT, "SIGINT"}, {SIGTERM, "SIGTERM"}};
            LOG_WRN << "terminal signal " << names[signal_number] << " received, terminating main context";
            shutdown();
        }
    });

    LOG_INF << "scheduling lifecycle participants ping";
    for (lifecycle_participant& x: lifecycle_participants_)
        context_.post([&x]{ x.up(); }); // TODO: VL: ping participants via their inboxes (once inboxes are implemented)
    context_.post([this]{LOG_INF << "UP";});

    LOG_INF << "starting lifecycle participants";
    for (lifecycle_participant& x: lifecycle_participants_)
    {
        LOG_INF << "starting " << dynamic_cast<entity&>(x).id();
        x.start();
    }

    work_ = std::make_unique<boost::asio::io_service::work>(context_);
    context_.run();

    std::reverse(begin(lifecycle_participants_), end(lifecycle_participants_));

    LOG_INF << "stopping lifecycle participants";
    for (lifecycle_participant& x: lifecycle_participants_)
    {
        LOG_INF << "stopping " << dynamic_cast<entity&>(x).id();
        x.stop();
    }

    LOG_INF << "deinitializing lifecycle participants";
    for (lifecycle_participant& x: lifecycle_participants_)
    {
        LOG_INF << "deinitializing " << dynamic_cast<entity&>(x).id();
        x.fini();
    }
}

void application::shutdown()
{
    work_.reset();
    context_.stop();
}

void application::load(application_config const& cfg)
{
    for (auto& entity_cfg: cfg.entities)
        add(entity_cfg.name, entity_cfg.loader_ref, entity_cfg.config);

    entities_.shrink_to_fit();

    for_each<lifecycle_participant>([&](lifecycle_participant& lp)
    {
        lifecycle_participants_.push_back(std::ref(lp));
    });

    lifecycle_participants_.shrink_to_fit();

    structure_locked_ = true;
}


} // namespace ufw
