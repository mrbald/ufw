/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 */
#include <ufw/core/sys/thread_usage.hpp>

#include <ctime>

#include <pthread.h>
#include <sys/resource.h>

#ifdef __APPLE__
#  include <mach/mach.h>
#endif

namespace ufw::core {

namespace {

constexpr std::uint64_t usec_to_ns = 1'000;
constexpr std::uint64_t sec_to_ns  = 1'000'000'000;

[[nodiscard]] std::uint64_t timeval_ns(timeval const& tv) noexcept
{
    return (static_cast<std::uint64_t>(tv.tv_sec) * sec_to_ns)
           + (static_cast<std::uint64_t>(tv.tv_usec) * usec_to_ns);
}

} // namespace

thread_cpu_handle current_thread_cpu_handle() noexcept
{
#ifdef __APPLE__
    // The pthread's kernel port WITHOUT an extra reference (nothing to deallocate).
    return {static_cast<std::uintptr_t>(pthread_mach_thread_np(pthread_self()))};
#else
    clockid_t clock_id{};
    if (pthread_getcpuclockid(pthread_self(), &clock_id) != 0)
    {
        return {};
    }
    return {static_cast<std::uintptr_t>(clock_id)};
#endif
}

thread_cpu_times sample_thread_cpu(thread_cpu_handle handle) noexcept
{
    if (handle.value == 0)
    {
        return {};
    }
#ifdef __APPLE__
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    auto const port = static_cast<thread_act_t>(handle.value);
    if (::thread_info(port, THREAD_BASIC_INFO,
                      reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS)
    {
        return {};
    }
    auto const user_ns = (static_cast<std::uint64_t>(info.user_time.seconds) * sec_to_ns)
                         + (static_cast<std::uint64_t>(info.user_time.microseconds) * usec_to_ns);
    auto const sys_ns = (static_cast<std::uint64_t>(info.system_time.seconds) * sec_to_ns)
                        + (static_cast<std::uint64_t>(info.system_time.microseconds) * usec_to_ns);
    return {.total_ns = user_ns + sys_ns, .system_ns = sys_ns};
#else
    timespec ts{};
    if (::clock_gettime(static_cast<clockid_t>(handle.value), &ts) != 0)
    {
        return {};
    }
    return {.total_ns = (static_cast<std::uint64_t>(ts.tv_sec) * sec_to_ns)
                        + static_cast<std::uint64_t>(ts.tv_nsec),
            .system_ns = 0}; // Linux cpuclock has no user/system split
#endif
}

process_usage_sample sample_process_usage() noexcept
{
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0)
    {
        return {};
    }
    // ru_maxrss: bytes on macOS, KILOBYTES on Linux — normalize to bytes.
#ifdef __APPLE__
    auto const maxrss_bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    // glibc wraps rusage's long fields in anonymous unions (32/64-bit compat) —
    // this is the kernel ABI struct, not a union of ours to variant-ify.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
    auto const maxrss_bytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
    return {.maxrss_bytes = maxrss_bytes,
            .user_ns      = timeval_ns(usage.ru_utime),
            .system_ns    = timeval_ns(usage.ru_stime)};
}

} // namespace ufw::core
