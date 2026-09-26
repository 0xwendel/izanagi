#include "entities/safe_read.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>

namespace izanagi::entities::detail {
namespace {

struct CacheEntry {
    std::uintptr_t page{};
    MEMORY_BASIC_INFORMATION info{};
    std::uint32_t generation{};
};

thread_local std::array<CacheEntry, 1024> g_pages;
thread_local std::uint32_t g_generation = 1;
thread_local bool g_batch = false;
thread_local ReadStats g_stats;

SIZE_T query(std::uintptr_t address, MEMORY_BASIC_INFORMATION& info) noexcept
{
    CacheEntry* entry = nullptr;
    std::uintptr_t page = 0;
    if (g_batch) {
        page = address >> 12;
        entry = &g_pages[page % g_pages.size()];
        if (entry->generation == g_generation && entry->page == page) {
            const auto base = reinterpret_cast<std::uintptr_t>(entry->info.BaseAddress);
            if (address >= base && address - base < entry->info.RegionSize) {
                info = entry->info;
                ++g_stats.cache_hits;
                return sizeof(info);
            }
        }
    }
    const auto start = std::chrono::steady_clock::now();
    const auto size = VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info));
    g_stats.query_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    ++g_stats.queries;
    if (entry && size == sizeof(info)) {
        entry->page = page;
        entry->info = info;
        entry->generation = g_generation;
    }
    return size;
}

bool user_address(std::uintptr_t address) noexcept
{
    return address >= 0x10000 && address < 0x0000800000000000ULL;
}

bool readable(std::uintptr_t address, std::size_t size) noexcept
{
    if (!user_address(address) || !size ||
        size > (std::numeric_limits<std::uintptr_t>::max)() - address ||
        !user_address(address + size - 1)) return false;
    const auto end = address + size;
    while (address < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (query(address, info) !=
                sizeof(info) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
        switch (info.Protect & 0xff) {
        case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY: break;
        default: return false;
        }
        const auto region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (region > address ||
            info.RegionSize > (std::numeric_limits<std::uintptr_t>::max)() - region ||
            region + info.RegionSize <= address) return false;
        address = (std::min)(end, region + info.RegionSize);
    }
    return true;
}

int memory_fault(unsigned code) noexcept
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ||
           code == STATUS_GUARD_PAGE_VIOLATION
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool copy_guarded(void* target, const void* source, std::size_t size) noexcept
{
    __try {
        std::memcpy(target, source, size);
        return true;
    } __except (memory_fault(GetExceptionCode())) {
        return false;
    }
}

}

ReadBatch::ReadBatch() noexcept : previous_(g_batch)
{
    if (!g_batch) {
        g_stats = {};
        ++g_generation;
        if (g_generation == 0) {
            for (auto& entry : g_pages) entry.generation = 0;
            g_generation = 1;
        }
    }
    g_batch = true;
}

ReadBatch::~ReadBatch() noexcept { g_batch = previous_; }

ReadStats BatchStats() noexcept { return g_stats; }

bool AddAddress(std::uintptr_t base, std::size_t offset,
                std::uintptr_t& result) noexcept
{
    if (base > (std::numeric_limits<std::uintptr_t>::max)() - offset) return false;
    result = base + offset;
    return user_address(result);
}

bool ReadMemory(std::uintptr_t address, void* target, std::size_t size) noexcept
{
    if (!target || !readable(address, size)) return false;
    return copy_guarded(target, reinterpret_cast<const void*>(address), size);
}

bool ReadName(std::uintptr_t address, char* target, std::size_t capacity) noexcept
{
    if (!target || capacity < 2 || capacity > 256) return false;
    char bytes[256]{};
    if (!ReadMemory(address, bytes, capacity)) return false;
    for (std::size_t i = 0; i < capacity; ++i) {
        const auto c = static_cast<unsigned char>(bytes[i]);
        if (c == 0) {
            if (i == 0) return false;
            std::memcpy(target, bytes, i + 1);
            return true;
        }
        if (c < 0x20 || c > 0x7e) return false;
    }
    return false;
}

bool ExecutableAddress(std::uintptr_t address) noexcept
{
    if (!user_address(address)) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (query(address, info) !=
            sizeof(info) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    switch (info.Protect & 0xff) {
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

}
