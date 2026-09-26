#include "source2/abi/schema_abi.hpp"

#include <Windows.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

namespace izanagi::source2::abi {
namespace {

constexpr std::uint32_t k_timestamp = 0x6ab198bc;
constexpr std::size_t k_image_size = 0x7f000;
constexpr std::size_t k_scope_method = 13;
constexpr std::size_t k_declared_class_method = 2;
constexpr std::size_t k_scope_name = 0x8;
constexpr std::size_t k_class_hash = 0x560;
constexpr std::size_t k_hash_buckets = 0x60;
constexpr std::size_t k_bucket_size = 0x18;
constexpr std::size_t k_bucket_count = 256;
constexpr std::size_t k_max_classes = 100000;
constexpr std::size_t k_max_fields = 8192;
constexpr std::size_t k_max_bases = 32;
thread_local std::size_t g_query_count = 0;
thread_local std::chrono::steady_clock::duration g_query_time{};

struct RegionCache {
    struct Entry {
        std::uintptr_t page{};
        MEMORY_BASIC_INFORMATION info{};
        bool valid{};
    };
    std::array<Entry, 1024> entries{};
    std::size_t page_size{};
    std::size_t hits{};

    RegionCache() noexcept
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        page_size = si.dwPageSize ? si.dwPageSize : 4096;
    }
};
thread_local RegionCache* g_cache = nullptr;

struct CacheSession {
    RegionCache* previous;
    explicit CacheSession(RegionCache& cache) noexcept : previous(g_cache) { g_cache = &cache; }
    ~CacheSession() noexcept { g_cache = previous; }
};

SIZE_T query_page(const void* address, MEMORY_BASIC_INFORMATION& mbi) noexcept
{
    const auto p = reinterpret_cast<std::uintptr_t>(address);
    RegionCache::Entry* entry = nullptr;
    std::uintptr_t page = 0;
    if (g_cache) {
        page = p / g_cache->page_size;
        entry = &g_cache->entries[page % g_cache->entries.size()];
        if (entry->valid && entry->page == page) {
            const auto base = reinterpret_cast<std::uintptr_t>(entry->info.BaseAddress);
            if (p >= base && p - base < entry->info.RegionSize) {
                mbi = entry->info;
                ++g_cache->hits;
                return sizeof(mbi);
            }
        }
    }
    const auto started = std::chrono::steady_clock::now();
    const auto result = VirtualQuery(address, &mbi, sizeof(mbi));
    g_query_time += std::chrono::steady_clock::now() - started;
    ++g_query_count;
    if (entry && result == sizeof(mbi)) {
        const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if (base <= p && mbi.RegionSize <=
                (std::numeric_limits<std::uintptr_t>::max)() - base &&
            p - base < mbi.RegionSize) {
            entry->page = page;
            entry->info = mbi;
            entry->valid = true;
        }
    }
    return result;
}

int memory_fault(unsigned code) noexcept
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ||
           code == STATUS_GUARD_PAGE_VIOLATION
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool try_copy(void* dst, const void* src, std::size_t size) noexcept
{
    __try {
        std::memcpy(dst, src, size);
        return true;
    } __except (memory_fault(GetExceptionCode())) {
        return false;
    }
}

bool page_readable(std::uintptr_t start, std::size_t size) noexcept
{
    if (!start || !size || start > (std::numeric_limits<std::uintptr_t>::max)() - size)
        return false;
    const auto end = start + size;
    while (start < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (query_page(reinterpret_cast<const void*>(start), mbi) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        const auto protect = mbi.Protect & 0xff;
        if (protect == PAGE_EXECUTE) return false;
        const auto region = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if (region > start || mbi.RegionSize > (std::numeric_limits<std::uintptr_t>::max)() - region ||
            region + mbi.RegionSize <= start) return false;
        start = (std::min)(end, region + mbi.RegionSize);
    }
    return true;
}

bool page_executable(const void* ptr) noexcept
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (query_page(ptr, mbi) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    switch (mbi.Protect & 0xff) {
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

bool in_image(const void* ptr, std::uintptr_t base, std::size_t size) noexcept
{
    const auto p = reinterpret_cast<std::uintptr_t>(ptr);
    return p >= base && p - base < size;
}

template <typename T>
bool copy_abi(std::uintptr_t address, T& out) noexcept
{
    if ((address & (alignof(T) - 1)) != 0 || !page_readable(address, sizeof(T)))
        return false;
    return try_copy(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

bool copy_name(const char* ptr, std::string& out, std::size_t limit = 255)
{
    out.clear();
    if (!ptr) return false;
    const auto p = reinterpret_cast<std::uintptr_t>(ptr);
    if (p > (std::numeric_limits<std::uintptr_t>::max)() - limit) return false;
    const auto end = p + limit;
    auto cursor = p;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (query_page(reinterpret_cast<const void*>(cursor), mbi) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
            (mbi.Protect & 0xff) == PAGE_EXECUTE) return false;
        const auto region = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        if (region > cursor ||
            mbi.RegionSize > (std::numeric_limits<std::uintptr_t>::max)() - region ||
            region + mbi.RegionSize <= cursor) return false;
        const auto chunk_end = (std::min)(end, region + mbi.RegionSize);
        std::array<char, 255> bytes{};
        const auto length = chunk_end - cursor;
        if (!try_copy(bytes.data(), reinterpret_cast<const void*>(cursor), length))
            return false;
        for (std::size_t i = 0; i < length; ++i) {
            const char c = bytes[i];
            if (c == '\0') return !out.empty();
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return false;
            out.push_back(c);
        }
        cursor = chunk_end;
    }
    return false;
}

bool method(void* object, std::size_t index, std::uintptr_t base,
            std::size_t image_size, void*& target) noexcept
{
    std::uintptr_t vtable{};
    const auto object_addr = reinterpret_cast<std::uintptr_t>(object);
    if (index > (std::numeric_limits<std::uintptr_t>::max)() / sizeof(void*) ||
        !copy_abi(object_addr, vtable) || !vtable ||
        vtable > (std::numeric_limits<std::uintptr_t>::max)() - index * sizeof(void*))
        return false;
    if (!copy_abi(vtable + index * sizeof(void*), target) ||
        !in_image(target, base, image_size) || !page_executable(target)) return false;
    return true;
}

// layout da abi inspecionada; estes offsets não são offsets de fields do jogo.
struct ClassHeader {
    std::uintptr_t binding;
    const char* name;
    const char* project_name;
    const char* cpp_name;
    std::int32_t size;
    std::uint16_t field_count;
    std::uint16_t metadata_count;
    std::uint8_t alignment; // 0xff é uma sentinela observada nesta build
    std::uint8_t base_count;
    std::uint16_t multiple_depth;
    std::uint16_t single_depth;
    std::uint16_t pad2e;
    std::uintptr_t fields;
    std::uintptr_t bases;
    std::uintptr_t data_desc;
    std::uintptr_t metadata;
    std::uintptr_t scope;
    std::uintptr_t type;
};
static_assert(sizeof(ClassHeader) == 0x60);
static_assert(offsetof(ClassHeader, bases) == 0x38);
static_assert(offsetof(ClassHeader, scope) == 0x50);

struct FieldHeader {
    const char* name;
    std::uintptr_t type;
    std::int32_t offset;
    std::int32_t metadata_count;
    std::uintptr_t metadata;
};
static_assert(sizeof(FieldHeader) == 0x20);

struct HashNode {
    std::uintptr_t key;
    std::uintptr_t next;
    std::uintptr_t data;
};
static_assert(sizeof(HashNode) == 0x18);

bool copy_class(std::uintptr_t address, std::uintptr_t expected_scope,
                std::string_view scope,
                schema::ClassInfo& out)
{
    ClassHeader header{};
    if (!copy_abi(address, header) || header.binding != address || header.size <= 0 ||
        header.size > (1 << 26) ||
        header.field_count > k_max_fields || header.base_count > k_max_bases ||
        (header.alignment != 0xff &&
         (header.alignment == 0 || header.alignment > 64 ||
          (header.alignment & (header.alignment - 1)) != 0)) ||
        header.scope != expected_scope ||
        (!header.fields && header.field_count != 0))
        return false;
    std::string project;
    if (!copy_name(header.project_name, project)) return false;
    out.scope = scope;
    if (!copy_name(header.name, out.name)) return false;
    out.size = static_cast<std::uint32_t>(header.size);
    out.fields.reserve(header.field_count);
    for (std::size_t i = 0; i < static_cast<std::size_t>(header.field_count); ++i) {
        if (header.fields > (std::numeric_limits<std::uintptr_t>::max)() -
                                i * sizeof(FieldHeader)) return false;
        FieldHeader field{};
        if (!copy_abi(header.fields + i * sizeof(FieldHeader), field) ||
            field.offset < 0 || field.offset >= header.size || !field.type)
            return false;
        schema::FieldInfo copied;
        copied.offset = static_cast<std::uint32_t>(field.offset);
        if (!copy_name(field.name, copied.name)) return false;
        std::uintptr_t type_name{};
        if (field.type > (std::numeric_limits<std::uintptr_t>::max)() - 0x8 ||
            !copy_abi(field.type + 0x8, type_name) ||
            !copy_name(reinterpret_cast<const char*>(type_name), copied.type_name))
            return false;
        out.fields.push_back(std::move(copied));
    }
    // descritor de base nesta build: offset em +0x0 e ponteiro da classe em +0x8.
    if (header.base_count && !header.bases) return false;
    for (std::size_t i = 0; i < header.base_count; ++i) {
        const auto at = header.bases + i * 0x10;
        if (at < header.bases ||
            at > (std::numeric_limits<std::uintptr_t>::max)() - 0x8) return false;
        std::uint32_t offset{};
        std::uintptr_t base_class{};
        std::uintptr_t name{};
        std::uintptr_t base_scope{};
        if (!copy_abi(at, offset) || !copy_abi(at + 0x8, base_class) ||
            !base_class || base_class > (std::numeric_limits<std::uintptr_t>::max)() -
                                            offsetof(ClassHeader, scope) ||
            !copy_abi(base_class + offsetof(ClassHeader, name), name) ||
            !copy_abi(base_class + offsetof(ClassHeader, scope), base_scope) ||
            !base_scope || base_scope > (std::numeric_limits<std::uintptr_t>::max)() -
                                            k_scope_name) return false;
        schema::BaseInfo base;
        base.offset = offset;
        if (!copy_name(reinterpret_cast<const char*>(name), base.name) ||
            !copy_name(reinterpret_cast<const char*>(base_scope + k_scope_name), base.scope) ||
            base.offset >= out.size) return false;
        out.bases.push_back(std::move(base));
    }
    return true;
}

}

bool MatchesProfile(std::uint32_t timestamp, std::size_t image_size) noexcept
{
    return timestamp == k_timestamp && image_size == k_image_size;
}

bool ValidateInterface(void* system, std::uintptr_t image_base,
                       std::size_t image_size) noexcept
{
    if (!system || (reinterpret_cast<std::uintptr_t>(system) & 7) != 0) return false;
    void* target{};
    return method(system, k_scope_method, image_base, image_size, target);
}

bool CopyScope(void* system, std::uintptr_t image_base, std::size_t image_size,
               std::string_view module, std::vector<schema::ClassInfo>& out) noexcept
{
    if (module.empty() || module.size() > 127 || module.find('\0') != std::string_view::npos)
        return false;
    try {
        RegionCache cache;
        CacheSession session(cache);
        g_query_count = 0;
        g_query_time = {};
        const auto started = std::chrono::steady_clock::now();
        void* fn{};
        if (!method(system, k_scope_method, image_base, image_size, fn)) return false;
        std::string name(module);
        using scope_fn = void* (__fastcall*)(void*, const char*, const char**);
        void* scope = reinterpret_cast<scope_fn>(fn)(system, name.c_str(), nullptr);
        if (!scope || (reinterpret_cast<std::uintptr_t>(scope) & 7) != 0) return false;
        const auto scope_address = reinterpret_cast<std::uintptr_t>(scope);
        if (scope_address > (std::numeric_limits<std::uintptr_t>::max)() -
                                k_class_hash - k_hash_buckets -
                                k_bucket_count * k_bucket_size) return false;
        std::string actual;
        if (!copy_name(reinterpret_cast<const char*>(
                           scope_address + k_scope_name), actual))
            return false;
        if (actual != name && actual != name.substr(0, name.find('.'))) return false;
        if (!method(scope, k_declared_class_method, image_base, image_size, fn)) return false;
        const auto scope_ready = std::chrono::steady_clock::now();
        const auto hash = reinterpret_cast<std::uintptr_t>(scope) + k_class_hash;
        std::int32_t allocated{};
        if (!copy_abi(hash + 0x0c, allocated) || allocated < 0 ||
            allocated > k_max_classes) return false;
        std::unordered_set<std::uintptr_t> visited;
        std::unordered_set<std::uintptr_t> bindings;
        for (std::size_t bucket = 0; bucket < k_bucket_count; ++bucket) {
            const auto at = hash + k_hash_buckets + bucket * k_bucket_size;
            std::uintptr_t node{};
            // a lista em +0x10 já inclui o sufixo consolidado; a outra não deve ser percorrida.
            if (!copy_abi(at + 0x10, node)) return false;
            while (node) {
                if (!visited.insert(node).second) return false;
                if (visited.size() > k_max_classes) return false;
                HashNode item{};
                if (!copy_abi(node, item)) return false;
                if (item.data) bindings.insert(item.data);
                node = item.next;
            }
        }
        if (bindings.empty() || bindings.size() > k_max_classes ||
            visited.size() != static_cast<std::size_t>(allocated) ||
            bindings.size() != visited.size()) return false;
        const auto hash_ready = std::chrono::steady_clock::now();
        // confere um binding do hash com a consulta pública antes do percurso.
        schema::ClassInfo first;
        if (!copy_class(*bindings.begin(), reinterpret_cast<std::uintptr_t>(scope),
                        name, first)) return false;
        const auto first_ready = std::chrono::steady_clock::now();
        // o retorno não trivial usa ponteiro oculto em rdx; o nome vai em r8.
        struct Handle { void* ptr; } declared{};
        static_assert(sizeof(Handle) == sizeof(void*));
        using declared_fn = Handle* (__fastcall*)(void*, Handle*, const char*);
        Handle* returned = reinterpret_cast<declared_fn>(fn)(scope, &declared,
                                                              first.name.c_str());
        if (returned != &declared ||
            declared.ptr != reinterpret_cast<void*>(*bindings.begin())) return false;
        const auto lookup_ready = std::chrono::steady_clock::now();
        std::vector<schema::ClassInfo> next;
        next.reserve(bindings.size());
        next.push_back(std::move(first));
        for (const auto binding : bindings) {
            if (binding == reinterpret_cast<std::uintptr_t>(declared.ptr)) continue;
            schema::ClassInfo cls;
            if (!copy_class(binding, reinterpret_cast<std::uintptr_t>(scope),
                            name, cls)) return false;
            next.push_back(std::move(cls));
        }
        out.insert(out.end(), std::make_move_iterator(next.begin()),
                   std::make_move_iterator(next.end()));
        const auto finished = std::chrono::steady_clock::now();
        const auto millis = [](auto from, auto to) noexcept {
            return std::chrono::duration<double, std::milli>(to - from).count();
        };
        (void)std::fprintf(stdout,
                           "izanagi: scope timing setup=%.1f ms hash=%.1f ms "
                           "first=%.1f ms lookup=%.1f ms classes=%.1f ms "
                           "virtualquery=%zu/%.1f ms cache_hits=%zu total=%.1f ms\n",
                           millis(started, scope_ready), millis(scope_ready, hash_ready),
                           millis(hash_ready, first_ready), millis(first_ready, lookup_ready),
                           millis(lookup_ready, finished), g_query_count,
                           std::chrono::duration<double, std::milli>(g_query_time).count(),
                           cache.hits, millis(started, finished));
        return true;
    } catch (...) { return false; }
}

}
