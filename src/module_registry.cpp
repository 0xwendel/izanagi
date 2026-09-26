#include "module_registry.hpp"

#include "pe_image.hpp"
#include <Psapi.h>
#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

namespace izanagi::modules {
namespace {

std::mutex g_lock;
std::vector<ModuleInfo> g_modules;
SnapshotData g_data;
bool g_initialized = false;

bool valid_name(std::wstring_view name) noexcept
{
    if (name.empty() || name.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    for (wchar_t c : name) {
        if (c == L'\0' || c == L'/' || c == L'\\') {
            return false;
        }
    }
    return true;
}

bool same_name(std::wstring_view a, std::wstring_view b) noexcept
{
    return a.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()) &&
           b.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()) &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool enumerate(std::vector<ModuleInfo>& result, SnapshotData& data) noexcept
{
    try {
        HANDLE process = GetCurrentProcess();
        std::vector<HMODULE> handles(128);
        DWORD needed = 0;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            if (!EnumProcessModulesEx(process, handles.data(),
                                      static_cast<DWORD>(handles.size() * sizeof(HMODULE)),
                                      &needed, LIST_MODULES_ALL)) {
                data.last_error = GetLastError();
                return false;
            }
            if (needed <= handles.size() * sizeof(HMODULE)) {
                handles.resize(needed / sizeof(HMODULE));
                break;
            }
            const auto count = static_cast<std::size_t>(needed) / sizeof(HMODULE) + 16;
            if (count > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()) /
                            sizeof(HMODULE)) {
                data.last_error = ERROR_BUFFER_OVERFLOW;
                return false;
            }
            handles.resize(count);
            if (attempt == 7) {
                data.last_error = ERROR_BUSY;
                return false;
            }
        }
        data.loaded_count = handles.size();
        for (HMODULE candidate : handles) {
            // Pin temporarily across GetModuleInformation and PE inspection.
            HMODULE pinned = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                    reinterpret_cast<LPCWSTR>(candidate), &pinned)) {
                continue; // module unloaded after enumeration
            }
            MODULEINFO info{};
            wchar_t name[MAX_PATH]{};
            const bool queried = GetModuleInformation(process, pinned, &info, sizeof(info)) &&
                                 GetModuleBaseNameW(process, pinned, name, MAX_PATH) != 0;
            if (queried) {
                const auto pe_info = pe::InspectImage(info.lpBaseOfDll, info.SizeOfImage);
                if (pe_info) {
                    result.push_back({name, pinned, pe_info->base, pe_info->image_size,
                                      pe_info->entry_point_rva, pe_info->timestamp,
                                      pe_info->sections});
                } else {
                    ++data.invalid_pe_count;
                }
            }
            FreeLibrary(pinned);
        }
        return true;
    } catch (...) {
        data.last_error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
}

}

bool Initialize() noexcept
{
    std::lock_guard lock(g_lock);
    if (g_initialized) return false;
    g_initialized = true;
    g_data = {};
    return true;
}

bool Refresh() noexcept
{
    {
        std::lock_guard lock(g_lock);
        if (!g_initialized) return false;
    }
    std::vector<ModuleInfo> next;
    SnapshotData data;
    const bool ok = enumerate(next, data);
    std::lock_guard lock(g_lock);
    if (!g_initialized) return false;
    if (ok) {
        data.generation = g_data.generation + 1;
        g_modules.swap(next);
        g_data = data;
    } else {
        g_modules.clear(); // stale handles are never retained as current
        g_data.loaded_count = 0;
        g_data.invalid_pe_count = 0;
        g_data.last_error = data.last_error;
        ++g_data.generation;
    }
    return ok;
}

void Shutdown() noexcept
{
    std::lock_guard lock(g_lock);
    g_initialized = false;
    std::vector<ModuleInfo>().swap(g_modules);
    g_data = {};
}

std::optional<ModuleInfo> Find(std::wstring_view name) noexcept
{
    if (!valid_name(name)) return std::nullopt;
    try {
        std::lock_guard lock(g_lock);
        if (!g_initialized) return std::nullopt;
        for (const auto& module : g_modules) {
            if (same_name(module.name, name)) return module;
        }
    } catch (...) {}
    return std::nullopt;
}

bool Contains(std::wstring_view name) noexcept { return Find(name).has_value(); }

SnapshotData Snapshot() noexcept
{
    std::lock_guard lock(g_lock);
    return g_data;
}

}
