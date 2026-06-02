/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Stage 1B: fixed-size SPSC ring<T> and multicast (pub/sub fan-out) ring<T>.
 * Tests are the executable spec. The concurrent tests pick a yield wait-policy on
 * a failed try_* (back-pressure is a signal; the wait is the caller's) so they make
 * progress under thread oversubscription (e.g. a 2-core CI runner) instead of a
 * non-yielding busy-spin starving the threads that can.
 */
#include <boost/test/unit_test.hpp>

#include <ufw/core/ring/multicast_buffer.hpp>
#include <ufw/core/ring/multicast_ring.hpp>
#include <ufw/core/ring/spsc_ring.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using ufw::core::multicast_buffer;
using ufw::core::multicast_ring;
using ufw::core::read_status;
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
            else
            {
                std::this_thread::yield();
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
        else
        {
            std::this_thread::yield();
        }
    }
    producer.join();
    BOOST_TEST(expected == count);
}

BOOST_AUTO_TEST_SUITE_END(/* ufw_core_spsc_ring */)

BOOST_AUTO_TEST_SUITE(ufw_core_multicast_ring)

BOOST_AUTO_TEST_CASE(reports_capacity_and_subscriber_count)
{
    multicast_ring<int> ring{5, 3};
    BOOST_TEST(ring.capacity() == 8u);
    BOOST_TEST(ring.subscribers() == 3u);
}

BOOST_AUTO_TEST_CASE(single_subscriber_sees_all_in_order)
{
    multicast_ring<int> ring{8, 1};
    auto sub = ring.subscribe();
    for (int i = 0; i < 6; ++i)
    {
        BOOST_REQUIRE(ring.try_push(i));
    }
    for (int i = 0; i < 6; ++i)
    {
        int v = -1;
        BOOST_REQUIRE(sub.try_pop(v));
        BOOST_REQUIRE_EQUAL(v, i);
    }
    int v = -1;
    BOOST_REQUIRE(!sub.try_pop(v));
}

// The defining multicast property: every subscriber independently sees the whole
// stream, in order.
BOOST_AUTO_TEST_CASE(every_subscriber_sees_every_record)
{
    multicast_ring<int> ring{8, 2};
    auto a = ring.subscribe();
    auto b = ring.subscribe();
    for (int i = 0; i < 6; ++i)
    {
        BOOST_REQUIRE(ring.try_push(i));
    }
    for (int i = 0; i < 6; ++i)
    {
        int va = -1;
        int vb = -1;
        BOOST_REQUIRE(a.try_pop(va));
        BOOST_REQUIRE(b.try_pop(vb));
        BOOST_REQUIRE_EQUAL(va, i);
        BOOST_REQUIRE_EQUAL(vb, i);
    }
}

// Loss-free: a slot is not reused until the SLOWEST subscriber has read it.
BOOST_AUTO_TEST_CASE(producer_is_gated_by_the_slowest_subscriber)
{
    multicast_ring<int> ring{4, 2};
    auto a = ring.subscribe();
    auto b = ring.subscribe();
    for (int i = 0; i < 4; ++i)
    {
        BOOST_REQUIRE(ring.try_push(i)); // fill capacity
    }
    BOOST_REQUIRE(!ring.try_push(99));   // full: gate = min(0, 0)

    int v = -1;
    for (int i = 0; i < 4; ++i)
    {
        BOOST_REQUIRE(a.try_pop(v)); // a races ahead and drains everything
        BOOST_REQUIRE_EQUAL(v, i);
    }
    BOOST_REQUIRE(!ring.try_push(99));   // STILL full: gated by b, still at 0

    BOOST_REQUIRE(b.try_pop(v));          // b reads exactly one
    BOOST_REQUIRE_EQUAL(v, 0);
    BOOST_REQUIRE(ring.try_push(99));     // one slot freed (gate = min(4, 1) = 1)
}

BOOST_AUTO_TEST_CASE(subscribing_past_the_reserved_count_throws)
{
    multicast_ring<int> ring{4, 1};
    [[maybe_unused]] auto const sub = ring.subscribe(); // claims the one reserved slot
    BOOST_CHECK_THROW((void)ring.subscribe(), std::out_of_range);
}

// One producer, N subscriber threads each reading the ENTIRE stream in order,
// under constant wrap + slowest-subscriber back-pressure. The real fan-out
// ordering/visibility test.
BOOST_AUTO_TEST_CASE(concurrent_multicast_every_subscriber_sees_full_stream)
{
    constexpr std::uint64_t count = 1U << 16;
    constexpr std::size_t subs = 4;
    multicast_ring<std::uint64_t> ring{1024, subs};

    std::atomic<int> failures{0};
    std::vector<std::thread> readers;
    for (std::size_t s = 0; s < subs; ++s)
    {
        readers.emplace_back([&ring, &failures]
        {
            auto sub = ring.subscribe();
            std::uint64_t value = 0;
            for (std::uint64_t expected = 0; expected < count;)
            {
                if (sub.try_pop(value))
                {
                    if (value != expected)
                    {
                        failures.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    ++expected;
                }
                else
                {
                    std::this_thread::yield();
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
        else
        {
            std::this_thread::yield();
        }
    }

    for (auto& t : readers)
    {
        t.join();
    }
    BOOST_TEST(failures.load() == 0);
}

BOOST_AUTO_TEST_SUITE_END(/* ufw_core_multicast_ring */)

BOOST_AUTO_TEST_SUITE(ufw_core_multicast_buffer)

BOOST_AUTO_TEST_CASE(reader_in_step_sees_every_record)
{
    multicast_buffer<int> buf{8};
    auto r = buf.subscribe();
    int out = 0;
    std::uint64_t skipped = 0;
    BOOST_REQUIRE(r.try_read(out, skipped) == read_status::empty); // nothing produced yet
    for (int i = 1; i <= 6; ++i)
    {
        buf.push(i * 100); // value at position i is i*100
    }
    for (int i = 1; i <= 6; ++i)
    {
        BOOST_REQUIRE(r.try_read(out, skipped) == read_status::ok);
        BOOST_REQUIRE_EQUAL(out, i * 100);
    }
    BOOST_REQUIRE(r.try_read(out, skipped) == read_status::empty); // caught up
}

BOOST_AUTO_TEST_CASE(independent_readers_each_see_the_stream)
{
    multicast_buffer<int> buf{8};
    auto a = buf.subscribe();
    auto b = buf.subscribe();
    for (int i = 1; i <= 5; ++i)
    {
        buf.push(i);
    }
    int va = 0;
    int vb = 0;
    std::uint64_t sk = 0;
    for (int i = 1; i <= 5; ++i)
    {
        BOOST_REQUIRE(a.try_read(va, sk) == read_status::ok);
        BOOST_REQUIRE(b.try_read(vb, sk) == read_status::ok);
        BOOST_REQUIRE_EQUAL(va, i);
        BOOST_REQUIRE_EQUAL(vb, i);
    }
}

// Value at position p is p. Fill the buffer (positions 1..4), then push one more
// (position 5) which overwrites position 1's slot. A reader still at position 1 must
// detect the lap, resync to the oldest still-readable (2), report exactly 1 skipped,
// then read 2..5 cleanly.
BOOST_AUTO_TEST_CASE(lapped_reader_resyncs_to_oldest_and_reports_the_gap)
{
    multicast_buffer<std::uint64_t> buf{4};
    BOOST_REQUIRE_EQUAL(buf.capacity(), 4u);
    auto r = buf.subscribe();
    for (std::uint64_t p = 1; p <= 4; ++p)
    {
        buf.push(p);
    }
    buf.push(5); // laps the slot holding position 1

    std::uint64_t out = 0;
    std::uint64_t skipped = 0;
    BOOST_REQUIRE(r.try_read(out, skipped) == read_status::lapped);
    BOOST_REQUIRE_EQUAL(skipped, 1u);          // only position 1 was lost
    BOOST_REQUIRE_EQUAL(r.position(), 2u);     // resynced to the oldest still in the buffer
    for (std::uint64_t expected = 2; expected <= 5; ++expected)
    {
        BOOST_REQUIRE(r.try_read(out, skipped) == read_status::ok);
        BOOST_REQUIRE_EQUAL(out, expected);
    }
    BOOST_REQUIRE(r.try_read(out, skipped) == read_status::empty);
}

// The real test: a producer overwrites freely (never waiting) while a reader drains.
// Value at position p is p, so every ok read MUST return its own position (no torn
// read slips past the seqlock), and every position is accounted for exactly once —
// read or skipped (read + skipped == produced).
BOOST_AUTO_TEST_CASE(concurrent_lossy_no_torn_reads_and_full_coverage)
{
    constexpr std::uint64_t count = 1U << 18;
    multicast_buffer<std::uint64_t> buf{1024};
    auto r = buf.subscribe();

    std::thread producer([&buf]
    {
        for (std::uint64_t p = 1; p <= count; ++p)
        {
            buf.push(p);
        }
    });

    std::uint64_t read_count = 0;
    std::uint64_t skipped_total = 0;
    std::uint64_t out = 0;
    std::uint64_t skipped = 0;
    while (r.position() <= count)
    {
        std::uint64_t const before = r.position();
        read_status const st = r.try_read(out, skipped);
        if (st == read_status::ok)
        {
            if (out != before)
            {
                BOOST_FAIL("torn read: got " << out << " at position " << before);
            }
            ++read_count;
        }
        else if (st == read_status::lapped)
        {
            skipped_total += skipped;
        }
        else
        {
            std::this_thread::yield();
        }
    }
    producer.join();
    BOOST_TEST(read_count + skipped_total == count); // every record read or skipped, once
}

BOOST_AUTO_TEST_SUITE_END(/* ufw_core_multicast_buffer */)

} // namespace
