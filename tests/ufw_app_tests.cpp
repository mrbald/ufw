/*
   Copyright 2017-2026 Vladimir Lysyy (mrbald@github)

   SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
*/

#include <boost/test/unit_test.hpp>

#include <ufw/app/actor.hpp>
#include <ufw/app/application.hpp>
#include <ufw/app/configuration.hpp>
#include <ufw/app/logger.hpp>

#include <yaml-cpp/yaml.h>

namespace {

// Start the Quill backend once for the whole test binary (the application logs).
struct logger_fixture
{
    logger_fixture() { ufw::initialize_logger(); }
};
BOOST_GLOBAL_FIXTURE(logger_fixture);

BOOST_AUTO_TEST_SUITE(ufw_app)

BOOST_AUTO_TEST_CASE(workers_and_assignment_config_decodes)
{
    auto const node = YAML::Load(R"(
workers:
  - id: 0
    loop: spinning
    pin_core: 2
  - id: 1
    loop: blocking
entities:
  - name: a
    worker: 1
)");
    auto const cfg = node.as<ufw::application_config>();
    BOOST_TEST(cfg.workers.size() == 2U);
    BOOST_TEST(cfg.workers[0].loop == "spinning");
    BOOST_TEST(cfg.workers[0].pin_core == 2U);
    BOOST_TEST(cfg.workers[1].loop == "blocking");
    BOOST_TEST(cfg.workers[1].pin_core == ~0U); // default: do not pin
    BOOST_TEST(cfg.entities.size() == 1U);
    BOOST_TEST(cfg.entities[0].worker == 1U);
}

BOOST_AUTO_TEST_CASE(telemetry_config_decodes)
{
    auto const node = YAML::Load(R"(
telemetry:
  file: app.metrics
  interval_ms: 250
entities: []
)");
    auto const cfg = node.as<ufw::application_config>();
    BOOST_TEST(cfg.telemetry.file == "app.metrics");
    BOOST_TEST(cfg.telemetry.interval_ms == 250U);
    BOOST_TEST(ufw::application_config{}.telemetry.file.empty()); // default: disabled
}

BOOST_AUTO_TEST_CASE(dispatch_flavour_config_decodes)
{
    auto const node = YAML::Load(R"(
dispatch: mpsc
entities: []
)");
    BOOST_TEST(node.as<ufw::application_config>().dispatch == "mpsc");
    BOOST_TEST(ufw::application_config{}.dispatch == "matrix"); // the default flavour
}

struct echo_actor final : ufw::entity, ufw::lifecycle_participant
{
    echo_actor(ufw::entity_id const& id, ufw::resolved_entity_id rid, ufw::application& app):
        ufw::entity{id, rid, app}, self_inbox{id, *this} {}

    void on_msg(int v) noexcept
    {
        sum += v;
        ++calls;
    }

    void init() override { self_inbox.resolve<&echo_actor::on_msg>(); }

    ufw::inbox_ref<void(int)> self_inbox;
    int sum = 0;
    int calls = 0;
};

// The single-thread collapse through the ENTITY layer: with no workers: block the
// inbox resolves to a DIRECT handle (worker 0 -> worker 0), a send runs
// synchronously, and no pool, rings, or threads exist at all.
BOOST_AUTO_TEST_CASE(inbox_ref_collapses_to_a_direct_call_without_a_pool)
{
    ufw::application app;
    app.add<echo_actor>("echo");
    app.load(ufw::application_config{}); // lock the structure (no pool configured)
    BOOST_TEST(!app.has_worker_pool());

    auto& actor = app.get<echo_actor>("echo");
    actor.init(); // the init() phase: resolve the inbox
    BOOST_TEST(static_cast<bool>(actor.self_inbox));

    actor.self_inbox(5);
    actor.self_inbox(7);
    BOOST_TEST(actor.sum == 12);
    BOOST_TEST(actor.calls == 2);
}

BOOST_AUTO_TEST_SUITE_END(/* ufw_app */)

} // local namespace
