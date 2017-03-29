#pragma once

#include "exception_types.hh"
#include "configuration.hh"
#include "logger.hh"

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

using entity_id = std::string;
using resolved_entity_id = size_t;

struct application;
struct lifecycle_participant;

#define ENTITY_LOGGER \
private:\
    mutable logger_t logger_ {[&]\
    {\
        logger_t logger;\
        logger.add_attribute("Entity", attrs::constant<entity_id>(id()));\
        return logger;\
    }()};\
public:\
    logger_t& get_logger() const { return logger_; }

struct entity
{
    entity(entity_id const& id, resolved_entity_id rid, application& app):
            id_{id}, rid_{rid}, app_{app} {}
    virtual ~entity() = default;

    entity_id const& id() const noexcept { return id_; }
    resolved_entity_id resolved_id() const noexcept { return rid_; }

    application& app() const noexcept { return app_; }

private:
    entity_id const id_;
    resolved_entity_id const rid_;
    application& app_;

    ENTITY_LOGGER;
    friend struct lifecycle_participant; // logger access
};

/**
 * Entity lazy references logic helper
 */
template <class T>
struct entity_ref
{
    T* operator->() const { return target_; }
    T* get() const { return target_; }

    explicit operator T&() { return *target_; }
    explicit operator T const&() const { return *target_; }

    explicit operator bool() const { return target_; }

    entity_ref(entity_id const& id, application& app):
            id_ {id},
            app_ {app} {}

    entity_id const& id() { return id_; }
    resolved_entity_id resolved_id() const { return resolved_id_; }

    void resolve();

private:
    entity_id const id_;
    application& app_;

    resolved_entity_id resolved_id_;
    T* target_ {};
};

struct lifecycle_participant
{
    // at this stage participants may discover and cache references (including strongly typed) to each other
    virtual void init() {}

    // at this stage participants may establish connections, spawn threads, etc.
    virtual void start() {}

    // at this stage participants may start messaging each other
    virtual void up() const noexcept final
    {
        auto* entity_ptr = dynamic_cast<entity const*>(this);
        if (entity_ptr)
        {
            auto const get_logger = [&]()->logger_t& { return entity_ptr->get_logger(); };
            LOG_INF << "UP";
        }
    }

    // reverse of start()
    virtual void stop() noexcept {}

    // reverse of init()
    virtual void fini() noexcept {}

    virtual ~lifecycle_participant() = default;
};

struct loader: entity
{
    using entity::entity;
    virtual std::unique_ptr<entity> load(entity_id const& id, resolved_entity_id rid, config_t const& cfg) = 0;
};


using loader_func_t = std::function<std::unique_ptr<entity>(config_t const& cfg, entity_id const& id, resolved_entity_id rid, application& app)>;

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

struct application
{
    application()
    {
        add<default_loader>("");

        register_loader("LOGGER", [&](config_t const& cfg, entity_id const& id, resolved_entity_id rid, application& app)
        {
            configure_logger(cfg.Scalar());
            return std::make_unique<entity>(id, rid, app);
        });
    }

    void register_loader(entity_id const& id, loader_func_t loader_func)
    {
        if (structure_locked_)
            throw fatal_error("cannot register loader - application structure already locked, likely a bug in the code");

        get<default_loader>("").register_loader(id, std::move(loader_func));
    }

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
    resolved_entity_id add(entity_id const& id, entity_id const& loader_id, config_t const& cfg)
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

    resolved_entity_id resolve_entity_id(entity_id const& id) const
    {
        auto it = entity_ids_.find(id);
        return it == entity_ids_.end() ? -1 : it->second;
    }

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

    entity& get(resolved_entity_id rid) const
    {
        return get<entity>(rid);
    }

    void load(int argc, char const** argv)
    {
        namespace po = boost::program_options;

        std::string config_file = "config.yaml";
        po::options_description desc {"Options"};
        desc.add_options()
            ("help,h", "Print this help message")
            ("config", po::value<std::string>(&config_file)->default_value(config_file), "application config file")
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

    void run()
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

    void shutdown()
    {
        work_.reset();
        context_.stop();
    }

    boost::asio::io_service& context() { return context_; }
private:
    entity_id id() const { return "app"; } // for ENTITY_LOGGER macro to work

private:
    void load(application_config const& cfg)
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
