/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 */
#include "memory_region.hpp"

#include <ufw/core/sys/compiler.hpp>

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Portable spelling of the anonymous-mapping flag.
#ifndef MAP_ANONYMOUS
#  define MAP_ANONYMOUS MAP_ANON
#endif

namespace ufw::core {

namespace {

int to_posix(prot p) noexcept
{
    switch (p)
    {
    case prot::none:       return PROT_NONE;
    case prot::read_only:  return PROT_READ;
    case prot::read_write: return PROT_READ | PROT_WRITE;
    }
    return PROT_NONE; // unreachable (all enumerators handled); silences -Wreturn-type
}

// Hoisted [[noreturn]] cold throw-helpers (sys/compiler.hpp has the policy): the
// call-site branches become statically unlikely, and the throw machinery stays out
// of the callers' inlining budgets and out of the warm text.
[[noreturn]] UFW_COLD UFW_NOINLINE void throw_errno(char const* what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

// For sites that must clean up first (close/munmap) and throw the SAVED errno.
[[noreturn]] UFW_COLD UFW_NOINLINE void throw_errno(int err, char const* what)
{
    throw std::system_error(err, std::generic_category(), what);
}

std::size_t round_up(std::size_t n, std::size_t multiple) noexcept
{
    return (n + multiple - 1) & ~(multiple - 1); // multiple is a power of two (page size)
}

} // namespace

std::size_t memory_region::page_size() noexcept
{
    return static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
}

// File-backed branch of the constructor: open/create the file, size it (writer)
// or take its size (reader), and MAP_SHARED it. Guard pages are excluded (they
// would punch PROT_NONE holes into someone's file view) and the huge-page hint is
// ignored (regular file mappings use base pages).
void memory_region::map_file(region_options const& opts)
{
    if (opts.guard_pages)
    {
        throw std::invalid_argument("memory_region: guard_pages is incompatible with a file mapping");
    }
    std::size_t const ps = page_size();

    bool const writer = opts.open_mode == file_mode::create_or_replace;
    if (writer && opts.bytes == 0)
    {
        throw std::invalid_argument("memory_region: create_or_replace requires bytes > 0");
    }
    if (!writer && opts.bytes != 0)
    {
        throw std::invalid_argument("memory_region: open_existing takes its size from the file (bytes must be 0)");
    }

    int oflags = opts.protection == prot::read_only ? O_RDONLY : O_RDWR;
    if (writer)
    {
        oflags |= O_CREAT;
    }
    int const fd = ::open(opts.file, oflags, 0644); // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (fd < 0)
    {
        throw_errno("memory_region: open");
    }

    std::size_t usable = 0;
    if (writer)
    {
        usable = round_up(opts.bytes, ps);
        if (::ftruncate(fd, static_cast<off_t>(usable)) != 0)
        {
            int const err = errno;
            ::close(fd);
            throw_errno(err, "memory_region: ftruncate");
        }
    }
    else
    {
        struct stat st{};
        if (::fstat(fd, &st) != 0 || st.st_size <= 0)
        {
            int const err = errno;
            ::close(fd);
            throw_errno(err, "memory_region: fstat/empty file");
        }
        usable = static_cast<std::size_t>(st.st_size);
    }

    void* base = ::mmap(nullptr, round_up(usable, ps), to_posix(opts.protection), MAP_SHARED, fd, 0);
    ::close(fd); // the mapping holds its own reference
    if (base == MAP_FAILED)
    {
        throw_errno("memory_region: mmap (file)");
    }

    base_ = static_cast<std::byte*>(base);
    span_ = round_up(usable, ps);
    data_ = base_;
    size_ = usable;

    if (opts.locking == lock::resident)
    {
        if (::mlock(data_, size_) != 0)
        {
            int const err = errno;
            reset();
            throw_errno(err, "memory_region: mlock");
        }
    }
}

memory_region::memory_region(region_options const& opts)
{
    if (opts.mapping == layout::mirrored)
    {
        throw std::runtime_error("memory_region: mirrored layout is not implemented until Stage 1C");
    }
    if (opts.file != nullptr)
    {
        map_file(opts);
        return;
    }
    if (opts.bytes == 0)
    {
        return; // empty region
    }

    std::size_t const ps = page_size();
    std::size_t const usable = round_up(opts.bytes, ps);
    std::size_t const guard = opts.guard_pages ? ps : 0;
    std::size_t const span = usable + (2 * guard);

    int flags = MAP_ANONYMOUS | MAP_PRIVATE;
    // Huge pages are a best-effort hint: Linux MAP_HUGETLB (only without guards,
    // to keep the size math simple), falling back to base pages on failure.
    // macOS superpages are not wired in Stage 1A — the hint degrades to base.
#if defined(__linux__) && defined(MAP_HUGETLB)
    bool const try_huge = opts.page_size != page::base && guard == 0;
    if (try_huge)
    {
        flags |= MAP_HUGETLB;
    }
#endif

    void* base = ::mmap(nullptr, span, to_posix(opts.protection), flags, -1, 0);
#if defined(__linux__) && defined(MAP_HUGETLB)
    if (base == MAP_FAILED && (flags & MAP_HUGETLB))
    {
        flags &= ~MAP_HUGETLB; // fall back to base pages
        base = ::mmap(nullptr, span, to_posix(opts.protection), flags, -1, 0);
    }
#endif
    if (base == MAP_FAILED)
    {
        throw_errno("memory_region: mmap");
    }

    base_ = static_cast<std::byte*>(base);
    span_ = span;
    data_ = base_ + guard;
    size_ = usable;

    if (guard != 0)
    {
        // Bracket the usable region with PROT_NONE pages so over/underruns trap.
        if (::mprotect(base_, guard, PROT_NONE) != 0 ||
            ::mprotect(data_ + size_, guard, PROT_NONE) != 0)
        {
            int const err = errno;
            reset();
            throw_errno(err, "memory_region: guard mprotect");
        }
    }

    if (opts.locking == lock::resident)
    {
        if (::mlock(data_, size_) != 0)
        {
            int const err = errno;
            reset();
            throw_errno(err, "memory_region: mlock");
        }
    }
}

memory_region::~memory_region()
{
    reset();
}

memory_region::memory_region(memory_region&& other) noexcept:
    data_{other.data_}, size_{other.size_}, base_{other.base_}, span_{other.span_}
{
    other.data_ = nullptr;
    other.size_ = 0;
    other.base_ = nullptr;
    other.span_ = 0;
}

memory_region& memory_region::operator=(memory_region&& other) noexcept
{
    if (this != &other)
    {
        reset();
        data_ = other.data_;
        size_ = other.size_;
        base_ = other.base_;
        span_ = other.span_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.base_ = nullptr;
        other.span_ = 0;
    }
    return *this;
}

void memory_region::reset() noexcept
{
    if (base_ != nullptr)
    {
        ::munmap(base_, span_);
    }
    data_ = nullptr;
    size_ = 0;
    base_ = nullptr;
    span_ = 0;
}

void memory_region::protect(std::span<std::byte> range, prot protection)
{
    if (range.empty())
    {
        return;
    }
    if (range.data() < data_ || range.data() + range.size() > data_ + size_)
    {
        throw std::out_of_range("memory_region::protect: range outside the region");
    }
    std::size_t const ps = page_size();
    // Page-align by pointer arithmetic (no int->ptr cast): floor to the page start,
    // round the length up to a whole page.
    std::size_t const head = reinterpret_cast<std::uintptr_t>(range.data()) & (ps - 1);
    std::byte* const floor = range.data() - head;
    std::size_t const len = round_up(static_cast<std::size_t>(range.data() + range.size() - floor), ps);
    if (::mprotect(floor, len, to_posix(protection)) != 0)
    {
        throw_errno("memory_region: mprotect");
    }
}

void memory_region::lock()
{
    if (!empty() && ::mlock(data_, size_) != 0)
    {
        throw_errno("memory_region: mlock");
    }
}

void memory_region::unlock()
{
    if (!empty() && ::munlock(data_, size_) != 0)
    {
        throw_errno("memory_region: munlock");
    }
}

} // namespace ufw::core
