/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 */
#include "memory_region.hpp"

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <sys/mman.h>
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

[[noreturn]] void throw_errno(char const* what)
{
    throw std::system_error(errno, std::generic_category(), what);
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

memory_region::memory_region(region_options opts)
{
    if (opts.mapping == layout::mirrored)
    {
        throw std::runtime_error("memory_region: mirrored layout is not implemented until Stage 1C");
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
            throw std::system_error(err, std::generic_category(), "memory_region: guard mprotect");
        }
    }

    if (opts.locking == lock::resident)
    {
        if (::mlock(data_, size_) != 0)
        {
            int const err = errno;
            reset();
            throw std::system_error(err, std::generic_category(), "memory_region: mlock");
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
