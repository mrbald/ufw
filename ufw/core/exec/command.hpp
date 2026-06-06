/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 *
 * The erased cross-worker invocation: "call this method on this object with these
 * args", flattened into one trivially-copyable POD a dispatch ring can carry. The
 * trampoline is a NON-VIRTUAL delegate (a free function bound at resolve time that
 * casts obj back to its concrete type and forwards the unpacked args); the args
 * travel INLINE, tightly packed — everything moves by memcpy, so alignment inside
 * the buffer is irrelevant and no padding is needed.
 *
 * Oversize or non-trivially-copyable args FAIL TO COMPILE (static_assert in
 * pack_args): a bigger payload must travel by its own ring or an interned handle —
 * a deliberate design decision at the call site, never a silent heap allocation.
 */
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ufw::core {

// Inline arg capacity. 40 makes sizeof(dispatch_cmd) == 64 — one x86-64 cache line
// per slot (half an Apple Silicon line). Tune empirically with the slot-size sweep
// in the dispatch benchmarks before trusting any other value.
inline constexpr std::size_t dispatch_arg_bytes = 40;

struct dispatch_cmd;

// The non-virtual delegate: bound once at resolve time, executed on the TARGET's
// worker thread by the column drainer.
using trampoline_t = void (*)(void* obj, dispatch_cmd const& cmd) noexcept;

struct dispatch_cmd
{
    using arg_buffer = std::array<std::byte, dispatch_arg_bytes>;

    trampoline_t  tramp;  // bound delegate (knows the arg types)
    void*         obj;    // the receiver, erased
    std::uint64_t stamp;  // enqueue tick (now_ticks) -> dispatch latency
    arg_buffer    args;   // tightly packed trivially-copyable args
};
static_assert(std::is_trivially_copyable_v<dispatch_cmd>); // valid spsc_ring slot
static_assert(sizeof(dispatch_cmd) == 64);                 // layout guard (see G3 sweep)

namespace detail {

// Tightly-packed arg offsets (prefix sums of sizes — no alignment, see file header).
template <class... Args>
[[nodiscard]] consteval std::array<std::size_t, sizeof...(Args)> arg_offsets() noexcept
{
    std::array<std::size_t, sizeof...(Args)> offsets{};
    std::size_t offset = 0;
    std::size_t i = 0;
    ((offsets[i++] = offset, offset += sizeof(Args)), ...);
    return offsets;
}

} // namespace detail

// Pack args into a command's arg buffer. Left-to-right (comma fold is sequenced).
template <class... Args>
void pack_args(dispatch_cmd::arg_buffer& buf, Args const&... args) noexcept
{
    static_assert((std::is_trivially_copyable_v<Args> && ...),
                  "dispatch args must be trivially copyable (ring slot constraint)");
    static_assert((std::size_t{0} + ... + sizeof(Args)) <= dispatch_arg_bytes,
                  "dispatch args exceed dispatch_arg_bytes -- give the payload its own ring "
                  "or pass an interned handle; the dispatch slot stays one cache line");
    std::size_t offset = 0;
    ((std::memcpy(buf.data() + offset, &args, sizeof(Args)), offset += sizeof(Args)), ...);
}

// Unpack Args from a command's arg buffer and invoke f(args...). Each read uses a
// compile-time offset (no shared mutation), so argument evaluation order is moot.
template <class... Args, class F>
decltype(auto) apply_from_buffer(dispatch_cmd::arg_buffer const& buf, F&& f) noexcept
{
    static_assert((std::is_trivially_copyable_v<Args> && ...));
    return [&]<std::size_t... I>(std::index_sequence<I...>) -> decltype(auto)
    {
        auto read_at = [&buf]<std::size_t J>(std::integral_constant<std::size_t, J>) noexcept
        {
            using arg_t = std::tuple_element_t<J, std::tuple<Args...>>;
            constexpr auto offsets = detail::arg_offsets<Args...>();
            std::array<std::byte, sizeof(arg_t)> tmp{};
            std::memcpy(tmp.data(), buf.data() + offsets[J], sizeof(arg_t));
            return std::bit_cast<arg_t>(tmp);
        };
        return std::forward<F>(f)(read_at(std::integral_constant<std::size_t, I>{})...);
    }(std::index_sequence_for<Args...>{});
}

} // namespace ufw::core
