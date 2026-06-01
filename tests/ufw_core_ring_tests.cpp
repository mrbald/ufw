/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Stage 1B-i: fixed-size SPSC ring<T>. Tests are the executable spec.
 */
#include <boost/test/unit_test.hpp>

#include <ufw/core/ring/spmc_ring.hpp>
#include <ufw/core/ring/spsc_ring.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

using ufw::core::spmc_ring;
using ufw::core::spsc_ring;

BOOST_AUTO_TEST_SUITE(ufw_core_spsc_ring)

BOOST_AUTO_TEST_CASE(capacity_rounds_up_to_a_power_of_two)
{
    BOOST_TEST(spsc_ring<int>{1}.capacity() == 1u);
    BOOST_TEST(spsc_ring<int>{5}.capacity() == 8u);
    BOOST_TEST(spsc_ring<int>{8}.capacity() == 8u);
    BOOST_TEST(spsc_ring<int>{9}.capacity() == 16u);
}

BOOST_AUTO_TEST_CASE(empty_ring_pops_nothing)
{
    spsc_ring<int> ring{4};
    int v = -1;
    BOOST_TEST(!ring.try_pop(v));
    BOOST_TEST(ring.try_peek() == nullptr);
}

BOOST_AUTO_TEST_CASE(single_push_then_pop_round_trips_the_value)
{
    spsc_ring<int> ring{4};
    BOOST_REQUIRE(ring.try_push(42));
    int v = -1;
    BOOST_REQUIRE(ring.try_pop(v));
    BOOST_TEST(v == 42);
    BOOST_TEST(!ring.try_pop(v)); // empty again
}

BOOST_AUTO_TEST_CASE(fills_to_capacity_then_back_pressures)
{
    spsc_ring<int> ring{4};
    BOOST_REQUIRE_EQUAL(ring.capacity(), 4u);
    for (int i = 0; i < 4; ++i)
    {
        BOOST_REQUIRE(ring.try_push(i)); // exactly capacity records fit
    }
    BOOST_REQUIRE(!ring.try_push(99));   // full -> back-pressure

    int v = -1;
    BOOST_REQUIRE(ring.try_pop(v));      // free exactly one
    BOOST_REQUIRE_EQUAL(v, 0);
    BOOST_REQUIRE(ring.try_push(99));    // now exactly one fits
    BOOST_REQUIRE(!ring.try_push(100));  // full again
}

BOOST_AUTO_TEST_CASE(preserves_fifo_order)
{
    spsc_ring<int> ring{8};
    for (int i = 0; i < 6; ++i)
    {
        BOOST_REQUIRE(ring.try_push(i));
    }
    for (int i = 0; i < 6; ++i)
    {
        int v = -1;
        BOOST_REQUIRE(ring.try_pop(v));
        BOOST_REQUIRE_EQUAL(v, i);
    }
}

BOOST_AUTO_TEST_CASE(wraps_around_many_laps)
{
    spsc_ring<std::uint64_t> ring{8};
    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < 8 * 100; ++i) // positions cross size and 2*size repeatedly
    {
        BOOST_REQUIRE(ring.try_push(i));
        BOOST_REQUIRE(ring.try_pop(v));
        BOOST_REQUIRE_EQUAL(v, i);
    }
}

BOOST_AUTO_TEST_CASE(zero_copy_claim_commit_peek_release)
{
    spsc_ring<int> ring{4};
    int* const w = ring.try_claim();
    BOOST_REQUIRE(w != nullptr);
    *w = 7;
    ring.commit();

    int const* const r = ring.try_peek();
    BOOST_REQUIRE(r != nullptr);
    BOOST_REQUIRE_EQUAL(*r, 7);
    ring.release();
    BOOST_TEST(ring.try_peek() == nullptr);
}

// The real ordering/visibility test: a producer and a consumer thread move a
// million sequence numbers through a small ring (forcing constant wrap + back-
// pressure); the consumer asserts strict FIFO and exact values.
BOOST_AUTO_TEST_CASE(concurrent_spsc_preserves_order_and_values)
{
    constexpr std::uint64_t count = 1U << 18; // hammers the wrap/back-pressure window many times over
    spsc_ring<std::uint64_t> ring{1024};

    std::thread producer([&ring]
    {
        for (std::uint64_t i = 0; i < count;)
        {
            if (ring.try_push(i))
            {
                ++i;
            }
        }
    });

    std::uint64_t expected = 0;
    std::uint64_t value = 0;
    while (expected < count)
    {
        if (ring.try_pop(value))
        {
            if (value != expected)
            {
                BOOST_FAIL("order/value mismatch: expected " << expected << " got " << value);
            }
            ++expected;
        }
    }
    producer.join();
    BOOST_TEST(expected == count);
}

BOOST_AUTO_TEST_SUITE_END(/* ufw_core_spsc_ring */)

BOOST_AUTO_TEST_SUITE(ufw_core_spmc_ring)

BOOST_AUTO_TEST_CASE(reports_capacity_and_consumer_count)
{
    spmc_ring<int> ring{5, 3};
    BOOST_TEST(ring.capacity() == 8u);
    BOOST_TEST(ring.consumers() == 3u);
}

BOOST_AUTO_TEST_CASE(single_consumer_is_fifo)
{
    spmc_ring<int> ring{8, 1};
    for (int i = 0; i < 6; ++i)
    {
        BOOST_REQUIRE(ring.try_push(i));
    }
    for (int i = 0; i < 6; ++i)
    {
        int v = -1;
        BOOST_REQUIRE(ring.try_consume(0, v));
        BOOST_REQUIRE_EQUAL(v, i);
    }
    int v = -1;
    BOOST_REQUIRE(!ring.try_consume(0, v)); // drained
}

// With one consumer the gate is a single sequence, so back-pressure is
// deterministic (with N>1 the producer is gated by the slowest consumer).
BOOST_AUTO_TEST_CASE(single_consumer_back_pressure_is_deterministic)
{
    spmc_ring<int> ring{4, 1};
    for (int i = 0; i < 4; ++i)
    {
        BOOST_REQUIRE(ring.try_push(i));
    }
    BOOST_REQUIRE(!ring.try_push(99)); // full
    int v = -1;
    BOOST_REQUIRE(ring.try_consume(0, v)); // free exactly one
    BOOST_REQUIRE_EQUAL(v, 0);
    BOOST_REQUIRE(ring.try_push(99));
    BOOST_REQUIRE(!ring.try_push(100));
}

// The work-sharing invariant: one producer, N competing consumers, every record
// is delivered to exactly one consumer — no loss, no duplication — under heavy
// wrap and back-pressure. Also the real multi-consumer ordering/visibility test.
BOOST_AUTO_TEST_CASE(every_record_is_consumed_exactly_once)
{
    constexpr std::uint64_t count = 1U << 16;
    constexpr std::size_t consumers = 4;
    spmc_ring<std::uint64_t> ring{1024, consumers};

    auto seen = std::make_unique<std::atomic<int>[]>(count); // zero-initialized
    std::atomic<std::uint64_t> consumed{0};

    std::vector<std::thread> pool;
    for (std::size_t i = 0; i < consumers; ++i)
    {
        pool.emplace_back([&ring, &seen, &consumed, i]
        {
            std::uint64_t value = 0;
            while (consumed.load(std::memory_order_relaxed) < count)
            {
                if (ring.try_consume(i, value))
                {
                    seen[value].fetch_add(1, std::memory_order_relaxed);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (std::uint64_t i = 0; i < count;)
    {
        if (ring.try_push(i))
        {
            ++i;
        }
    }

    for (auto& t : pool)
    {
        t.join();
    }

    BOOST_TEST(consumed.load() == count);
    for (std::uint64_t v = 0; v < count; ++v)
    {
        if (seen[v].load() != 1)
        {
            BOOST_FAIL("value " << v << " consumed " << seen[v].load() << " times (expected 1)");
        }
    }
}

BOOST_AUTO_TEST_SUITE_END(/* ufw_core_spmc_ring */)

} // namespace
