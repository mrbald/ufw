/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * Stage 1A: memory_region. Tests written before the implementation.
 */
#include <boost/test/unit_test.hpp>

#include <ufw/core/mem/memory_region.hpp>

#include <csignal>
#include <cstddef>
#include <cstring>
#include <span>
#include <system_error>

#include <sys/wait.h>
#include <unistd.h>

// Fault (death) tests fork and trap a SIGSEGV/SIGBUS. A sanitizer's own fault
// handler makes that unstable, so the build defines UFW_TESTS_SANITIZED=1 when
// sanitizers are on and we skip them (they run in the non-sanitized build).
#ifndef UFW_TESTS_SANITIZED
#  define UFW_TESTS_SANITIZED 0
#endif

namespace {

using ufw::core::memory_region;
using ufw::core::prot;
using ufw::core::lock;

bool is_pow2(std::size_t n) { return n != 0 && (n & (n - 1)) == 0; }

// Run `fn` in a forked child; return true iff the child terminated abnormally
// (killed by a signal, or non-zero exit). Used to assert a fault actually traps.
template <class F>
bool faults_in_child(F&& fn)
{
    pid_t const pid = ::fork();
    if (pid == 0)
    {
        // Reset to default disposition so the fault terminates the child cleanly,
        // rather than being caught by Boost.Test's monitor (inherited via fork).
        ::signal(SIGSEGV, SIG_DFL);
        ::signal(SIGBUS, SIG_DFL);
        std::forward<F>(fn)();
        ::_exit(0); // reached only if no fault occurred
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFSIGNALED(status); // killed by a signal => the access trapped
}

BOOST_AUTO_TEST_SUITE(ufw_core_memory_region)

BOOST_AUTO_TEST_CASE(page_size_is_a_sane_power_of_two)
{
    std::size_t const ps = memory_region::page_size();
    BOOST_TEST(is_pow2(ps));
    BOOST_TEST(ps >= 4096u); // 4096 on typical Linux, 16384 on Apple Silicon
}

BOOST_AUTO_TEST_CASE(maps_a_writable_region_rounded_up_to_a_page)
{
    memory_region const r{{.bytes = 1}};
    BOOST_TEST(r.size() == memory_region::page_size());
    BOOST_REQUIRE(r.data() != nullptr);
    BOOST_TEST(!r.empty());
}

BOOST_AUTO_TEST_CASE(rounds_to_page_multiples)
{
    std::size_t const ps = memory_region::page_size();
    BOOST_TEST(memory_region{{.bytes = ps}}.size() == ps);
    BOOST_TEST(memory_region{{.bytes = ps + 1}}.size() == 2 * ps);
}

BOOST_AUTO_TEST_CASE(zero_bytes_is_an_empty_region)
{
    memory_region const r{{.bytes = 0}};
    BOOST_TEST(r.empty());
    BOOST_TEST(r.size() == 0u);
    BOOST_TEST(r.data() == nullptr);
}

BOOST_AUTO_TEST_CASE(region_is_readable_and_writable)
{
    memory_region r{{.bytes = 4096}};
    std::span<std::byte> const m{r.data(), r.size()};
    std::memset(m.data(), 0xAB, m.size());
    BOOST_TEST(std::to_integer<int>(m[0]) == 0xAB);
    BOOST_TEST(std::to_integer<int>(m[m.size() - 1]) == 0xAB);
}

BOOST_AUTO_TEST_CASE(move_transfers_ownership_and_empties_the_source)
{
    memory_region a{{.bytes = 4096}};
    std::byte* const original = a.data();
    memory_region b{std::move(a)};
    BOOST_TEST(b.data() == original);
    BOOST_TEST(a.empty());      // NOLINT(bugprone-use-after-move) — asserting the moved-from state
    BOOST_TEST(a.data() == nullptr);
}

BOOST_AUTO_TEST_CASE(lock_and_unlock_a_small_resident_region)
{
    // mlock is gated by RLIMIT_MEMLOCK / CAP_IPC_LOCK. macOS lets an unprivileged
    // user pin a few pages, but many Linux CI sandboxes set the limit to 0, so the
    // call is refused (EPERM/ENOMEM/EAGAIN). That is an environment limit, not a
    // memory_region bug — the ctor is *right* to throw it. Tolerate denial as a
    // skip here; assert the lock/unlock round-trip only where locking is permitted.
    try
    {
        memory_region r{{.bytes = 4096, .locking = lock::resident}}; // mlock at construction
        BOOST_CHECK_NO_THROW(r.unlock());
        BOOST_CHECK_NO_THROW(r.lock());
        BOOST_CHECK_NO_THROW(r.unlock());
    }
    catch (std::system_error const& e)
    {
        if (e.code() != std::errc::operation_not_permitted &&      // EPERM
            e.code() != std::errc::not_enough_memory &&            // ENOMEM
            e.code() != std::errc::resource_unavailable_try_again) // EAGAIN
        {
            throw; // an unexpected errno is a genuine failure — let it fail the test
        }
        BOOST_TEST_MESSAGE("mlock not permitted in this environment — skipping: " << e.what());
    }
}

BOOST_AUTO_TEST_CASE(construct_destruct_loop_does_not_leak_mappings)
{
    // Under ASan this catches a leaked mapping/fd; without it, address-space exhaustion.
    for (int i = 0; i < 4096; ++i)
    {
        memory_region r{{.bytes = 64 * 1024}};
        BOOST_REQUIRE(r.data() != nullptr);
        r.data()[0] = std::byte{1};
    }
    BOOST_TEST(true);
}

#if !UFW_TESTS_SANITIZED
BOOST_AUTO_TEST_CASE(protect_read_only_then_write_faults)
{
    memory_region r{{.bytes = 4096}};
    r.protect({r.data(), r.size()}, prot::read_only);
    BOOST_TEST(faults_in_child([&] { r.data()[0] = std::byte{1}; }));
}

BOOST_AUTO_TEST_CASE(guard_pages_trap_overrun_and_underrun)
{
    memory_region r{{.bytes = 4096, .guard_pages = true}};
    std::size_t const ps = memory_region::page_size();
    r.data()[0] = std::byte{1};               // usable region is fine
    r.data()[r.size() - 1] = std::byte{1};
    BOOST_TEST(faults_in_child([&] { r.data()[r.size() + ps / 2] = std::byte{1}; })); // overrun -> guard
    BOOST_TEST(faults_in_child([&] {
        // underrun: touch the page just before the usable region
        *(r.data() - ps / 2) = std::byte{1}; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }));
}
#endif // !UFW_TESTS_SANITIZED

BOOST_AUTO_TEST_SUITE_END(/* ufw_core_memory_region */)

} // namespace
