/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ufw::core {

// One simple concept: an OS memory region owned by RAII. The option surface
// anticipates the features the rings will want — locking pages resident,
// changing protection, guard pages, huge pages, and a double-mapped ("mirrored")
// layout — without the ring having to know how any of them are done.
enum class prot   : std::uint8_t { none, read_only, read_write };
enum class lock   : std::uint8_t { none, resident };            // mlock the pages
enum class page   : std::uint8_t { base, huge_2mb, huge_1gb };  // best-effort hint; falls back to base
enum class layout : std::uint8_t { plain, mirrored };           // mirrored = mapped twice contiguously (Stage 1C)

// File-backed regions only (the telemetry substrate): how the file is obtained.
enum class file_mode : std::uint8_t
{
    create_or_replace, // writer: open/create + ftruncate to the page-rounded `bytes`
    open_existing,     // reader: size comes from the file (`bytes` must be 0)
};

struct region_options
{
    std::size_t bytes = 0;                       // rounded up to a page multiple; 0 = empty region
    prot   protection  = prot::read_write;
    lock   locking     = lock::none;
    page   page_size   = page::base;
    layout mapping     = layout::plain;
    bool   guard_pages = false;                  // PROT_NONE pages bracketing the usable region

    // Non-null => a FILE-BACKED MAP_SHARED mapping (cross-process visible, survives
    // the process for post-mortem reads — the telemetry handoff boundary). File
    // mappings exclude guard_pages and ignore the huge-page hint. The pointer only
    // needs to outlive the constructor call.
    char const* file = nullptr;
    file_mode   open_mode = file_mode::create_or_replace;
};

// Move-only RAII over an mmap'd region. Throws std::system_error (with errno) on
// any mapping failure. No dependency on the rest of the framework.
class memory_region
{
public:
    explicit memory_region(region_options const& opts);
    ~memory_region();

    memory_region(memory_region&& other) noexcept;
    memory_region& operator=(memory_region&& other) noexcept;

    memory_region(memory_region const&) = delete;
    memory_region& operator=(memory_region const&) = delete;

    [[nodiscard]] std::byte*  data() const noexcept { return data_; }  // usable region start
    [[nodiscard]] std::size_t size() const noexcept { return size_; }  // usable bytes (page-rounded)
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    // Change protection on a sub-range (page-granular: addr rounded down, end up).
    // The range must lie within [data(), data()+size()).
    void protect(std::span<std::byte> range, prot protection);

    // Lock/unlock the usable region in RAM (mlock/munlock).
    void lock();
    void unlock();

    // The OS page size actually in effect — sysconf(_SC_PAGESIZE). 16384 on Apple
    // Silicon, 4096 on typical Linux; never assume a constant.
    [[nodiscard]] static std::size_t page_size() noexcept;

private:
    void map_file(region_options const& opts);    // the file-backed constructor branch
    void reset() noexcept;                        // munmap the whole mapping

    std::byte*  data_ = nullptr;                  // usable region start (== base_ unless guarded)
    std::size_t size_ = 0;                         // usable bytes
    std::byte*  base_ = nullptr;                   // mapping base
    std::size_t span_ = 0;                         // total mapped bytes (guards + usable)
};

} // namespace ufw::core
