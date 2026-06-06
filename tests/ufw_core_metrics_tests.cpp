/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * The telemetry substrate: the mmap gauge file. The cross-process proof is done
 * the honest way — write through the registry, then open a SECOND, read-only
 * mapping of the same file and parse the bytes by hand, exactly as the external
 * tools/metrics.py reader does.
 */
#include <boost/test/unit_test.hpp>

#include <ufw/core/mem/memory_region.hpp>
#include <ufw/core/metrics/metrics.hpp>
#include <ufw/core/sys/timing.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace {

namespace mx = ufw::core::metrics;

BOOST_AUTO_TEST_SUITE(ufw_core_metrics)

BOOST_AUTO_TEST_CASE(layout_is_the_published_contract)
{
    BOOST_TEST(sizeof(mx::file_header) == 64U);
    BOOST_TEST(sizeof(mx::file_entry) == 128U);
    BOOST_TEST(sizeof(mx::value_str_payload) == 64U);
    BOOST_TEST(std::is_trivially_copyable_v<mx::series_payload>);
}

BOOST_AUTO_TEST_CASE(histogram_buckets_are_sane)
{
    // linear below 16
    BOOST_TEST(mx::hist_bucket_index(0) == 0U);
    BOOST_TEST(mx::hist_bucket_index(15) == 15U);
    // log-linear above; monotone non-decreasing in the value
    BOOST_TEST(mx::hist_bucket_index(16) >= 16U);
    BOOST_TEST(mx::hist_bucket_index(1U << 20U) > mx::hist_bucket_index(1U << 10U));
    BOOST_TEST(mx::hist_bucket_index(~std::uint64_t{0}) < mx::hist_buckets);
    // values within one 1/16 step share a bucket
    BOOST_TEST(mx::hist_bucket_index(1024) == mx::hist_bucket_index(1024 + 31));
}

BOOST_AUTO_TEST_CASE(registration_guards_capacity_and_duplicates)
{
    mx::registry reg{{.max_entries = 2}};
    auto const c = reg.make_counter("a_total");
    BOOST_TEST(static_cast<bool>(c));
    BOOST_CHECK_THROW((void)reg.make_counter("a_total"), std::invalid_argument); // duplicate
    (void)reg.make_value_i64("b");
    BOOST_CHECK_THROW((void)reg.make_counter("c_total"), std::length_error); // capacity
    BOOST_TEST(reg.size() == 2U);
}

BOOST_AUTO_TEST_CASE(null_handles_are_inert)
{
    mx::counter const c{};
    mx::series const s{};
    c.add();             // must be safe no-ops (telemetry wired unconditionally,
    s.record(1, 2);      //  enabled by config)
    BOOST_TEST(!static_cast<bool>(c));
}

// The full round trip: write gauges through the registry into a FILE, then parse a
// second read-only mapping by hand (the external-reader contract).
BOOST_AUTO_TEST_CASE(gauges_round_trip_through_the_file)
{
    char const* const path = "ufw_metrics_test.bin";
    {
        mx::registry reg{{.file = path, .max_entries = 8, .arena_bytes = 64U << 10U}};
        auto const events = reg.make_counter(R"(ufw_events_total{worker="0"})");
        auto const depth  = reg.make_value_i64("ufw_queue_depth");
        auto const name   = reg.make_value_str("ufw_build");
        auto const lat    = reg.make_series("ufw_latency_ns");

        events.add();
        events.add(2);
        depth.set(-7);
        name.set("phase2");
        for (std::uint64_t v : {100U, 200U, 300U, 400U, 1'000'000U})
        {
            lat.record(v, ufw::core::now_ticks());
        }

        // parse a SECOND mapping while the writer mapping is still live
        ufw::core::memory_region const ro{{.protection = ufw::core::prot::read_only,
                                           .file = path,
                                           .open_mode = ufw::core::file_mode::open_existing}};
        auto const* header = reinterpret_cast<mx::file_header const*>(ro.data());
        BOOST_REQUIRE_EQUAL(header->magic, mx::file_magic);
        BOOST_REQUIRE_EQUAL(header->version, mx::file_version);
        BOOST_REQUIRE_EQUAL(header->entry_count, 4U);

        auto const* entries =
            reinterpret_cast<mx::file_entry const*>(ro.data() + sizeof(mx::file_header));

        BOOST_TEST(std::string{entries[0].name.data()} == R"(ufw_events_total{worker="0"})");
        BOOST_TEST(entries[0].type == static_cast<std::uint32_t>(mx::gauge_type::counter));
        BOOST_TEST(*reinterpret_cast<std::uint64_t const*>(ro.data() + entries[0].payload_offset) == 3U);

        BOOST_TEST(entries[1].type == static_cast<std::uint32_t>(mx::gauge_type::value_i64));
        BOOST_TEST(*reinterpret_cast<std::int64_t const*>(ro.data() + entries[1].payload_offset) == -7);

        BOOST_TEST(entries[2].type == static_cast<std::uint32_t>(mx::gauge_type::value_str));
        auto const* sp =
            reinterpret_cast<mx::value_str_payload const*>(ro.data() + entries[2].payload_offset);
        BOOST_TEST(sp->seq % 2 == 0U);
        BOOST_TEST(std::string{sp->chars.data()} == "phase2");

        BOOST_TEST(entries[3].type == static_cast<std::uint32_t>(mx::gauge_type::series));
        auto const* series =
            reinterpret_cast<mx::series_payload const*>(ro.data() + entries[3].payload_offset);
        BOOST_TEST(series->seq % 2 == 0U);
        BOOST_TEST(series->count == 5U);
        BOOST_TEST(series->sum == 1'001'000U);
        BOOST_TEST(series->min == 100U);
        BOOST_TEST(series->max == 1'000'000U);
        std::uint64_t const bucketed =
            std::accumulate(series->buckets.begin(), series->buckets.end(), std::uint64_t{0});
        BOOST_TEST(bucketed == series->count); // every sample landed in exactly one bucket
        bool worst_has_max = false;
        for (std::uint64_t const w : series->worst_vals)
        {
            worst_has_max = worst_has_max || w == 1'000'000U;
        }
        BOOST_TEST(worst_has_max); // the spike is retained as an exemplar
    }
    BOOST_TEST(::unlink(path) == 0);
}

BOOST_AUTO_TEST_SUITE_END(/* ufw_core_metrics */)

} // namespace
