#include "entities/entity_system_adapter.hpp"

#include "entities/safe_read.hpp"
#include "pe_image.hpp"
#include <Psapi.h>
#include <array>
#include <cstring>
#include <utility>

namespace izanagi::entities::detail {
namespace {

constexpr std::uint32_t k_engine_timestamp = 0x6ab58b8e;
constexpr std::size_t k_engine_size = 0x971000;
constexpr std::uint32_t k_client_timestamp = 0x6ab6d1db;
constexpr std::size_t k_client_size = 0x2998000;
constexpr std::uint32_t k_max_entities = 64 * 512;
constexpr std::size_t k_identity_size = 0x70;
constexpr std::size_t k_chunk_size = 512 * k_identity_size;
constexpr std::uint32_t k_invalid_flags = 0x1 | 0x10 | 0x200 | 0x400;

template <typename T>
bool read_at(std::uintptr_t base, std::size_t offset, T& result) noexcept
{
    std::uintptr_t address{};
    return AddAddress(base, offset, address) &&
           ReadMemory(address, &result, sizeof(result));
}

bool inside(std::uintptr_t address, const modules::ModuleInfo& module) noexcept
{
    return address >= module.base && address - module.base < module.image_size;
}

bool pinned_matches(HMODULE pinned, const modules::ModuleInfo& expected) noexcept
{
    MODULEINFO mapped{};
    if (!GetModuleInformation(GetCurrentProcess(), pinned, &mapped, sizeof(mapped)))
        return false;
    const auto image = pe::InspectImage(mapped.lpBaseOfDll, mapped.SizeOfImage);
    return image && image->base == expected.base &&
           image->timestamp == expected.timestamp &&
           image->image_size == expected.image_size;
}

std::uint32_t word32(const std::byte* bytes, std::size_t offset) noexcept
{
    std::uint32_t result{};
    std::memcpy(&result, bytes + offset, sizeof(result));
    return result;
}

std::uintptr_t wordptr(const std::byte* bytes, std::size_t offset) noexcept
{
    std::uintptr_t result{};
    std::memcpy(&result, bytes + offset, sizeof(result));
    return result;
}

}

EntitySystemAdapter::EntitySystemAdapter(interfaces::Lease engine, HMODULE client,
                                         modules::ModuleInfo engine_info,
                                         modules::ModuleInfo client_info) noexcept
    : engine_(std::move(engine)), client_(client),
      engine_info_(std::move(engine_info)), client_info_(std::move(client_info)) {}

EntitySystemAdapter::~EntitySystemAdapter() noexcept
{
    if (client_) FreeLibrary(client_);
}

std::unique_ptr<EntitySystemAdapter> EntitySystemAdapter::Acquire(Failure& failure) noexcept
{
    failure = Failure::module_missing;
    try {
        const auto engine_info = modules::Find(L"engine2.dll");
        const auto client_info = modules::Find(L"client.dll");
        if (!engine_info || !client_info) return {};
        failure = Failure::profile_mismatch;
        if (engine_info->timestamp != k_engine_timestamp ||
            engine_info->image_size != k_engine_size ||
            client_info->timestamp != k_client_timestamp ||
            client_info->image_size != k_client_size) return {};
        failure = Failure::interface_unavailable;
        auto engine = interfaces::Resolve(L"engine2.dll",
                                          "GameResourceServiceClientV001");
        if (!engine || engine.base() != engine_info->base ||
            !pinned_matches(reinterpret_cast<HMODULE>(engine.base()), *engine_info))
            return {};
        failure = Failure::module_changed;
        HMODULE client = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(client_info->handle), &client))
            return {};
        if (!pinned_matches(client, *client_info)) {
            FreeLibrary(client);
            return {};
        }
        auto adapter = std::unique_ptr<EntitySystemAdapter>(
            new EntitySystemAdapter(std::move(engine), client, *engine_info,
                                    *client_info));
        failure = Failure::system_unavailable;
        if (!adapter->Available()) return {};
        failure = Failure::none;
        return adapter;
    } catch (...) { failure = Failure::exception; return {}; }
}

bool EntitySystemAdapter::Current() const noexcept
{
    const auto engine = modules::Find(L"engine2.dll");
    const auto client = modules::Find(L"client.dll");
    return engine && client && engine->base == engine_info_.base &&
           engine->timestamp == engine_info_.timestamp &&
           engine->image_size == engine_info_.image_size &&
           client->base == client_info_.base &&
           client->timestamp == client_info_.timestamp &&
           client->image_size == client_info_.image_size;
}

bool EntitySystemAdapter::System(std::uintptr_t& result) const noexcept
{
    const auto service = reinterpret_cast<std::uintptr_t>(engine_.get());
    std::uintptr_t service_vtable{};
    std::uintptr_t service_method{};
    if (!read_at(service, 0, service_vtable) ||
        !inside(service_vtable, engine_info_) ||
        !read_at(service_vtable, 0, service_method) ||
        !inside(service_method, engine_info_) ||
        !ExecutableAddress(service_method) ||
        !read_at(service, 0x58, result) || !result || (result & 7) != 0)
        return false;
    std::uintptr_t vtable{};
    std::uintptr_t method{};
    if (!read_at(result, 0, vtable) || !inside(vtable, client_info_) ||
        !read_at(vtable, 0, method) || !inside(method, client_info_) ||
        !ExecutableAddress(method)) return false;
    std::int32_t used{};
    std::int32_t dormant{};
    if (!read_at(result, 0x228, used) || !read_at(result, 0x240, dormant) ||
        used < 0 || dormant < 0 ||
        used > static_cast<std::int32_t>(k_max_entities) ||
        dormant > static_cast<std::int32_t>(k_max_entities) ||
        used + dormant > static_cast<std::int32_t>(k_max_entities)) return false;
    for (std::size_t i = 0; i < 64; ++i) {
        std::uintptr_t chunk{};
        if (!read_at(result, 0x10 + i * sizeof(void*), chunk)) return false;
        if (!chunk) continue;
        if ((chunk & 7) != 0) return false;
        std::uint64_t count{};
        return chunk >= sizeof(count) &&
               ReadMemory(chunk - sizeof(count), &count, sizeof(count)) &&
               count == 512;
    }
    return used + dormant == 0;
}

bool EntitySystemAdapter::Available() const noexcept
{
    std::uintptr_t system{};
    return Current() && System(system);
}

bool EntitySystemAdapter::Slot(std::uintptr_t system, std::uint32_t index,
                               std::uintptr_t& result) const noexcept
{
    if (index >= k_max_entities) return false;
    std::uintptr_t chunk{};
    if (!read_at(system, 0x10 + (index / 512) * sizeof(void*), chunk) ||
        !chunk || (chunk & 7) != 0)
        return false;
    return AddAddress(chunk, (index % 512) * k_identity_size, result);
}

bool EntitySystemAdapter::Normalize(std::uintptr_t address, const std::byte* bytes,
                                    std::uint32_t index, LiveIdentity& out,
                                    std::unordered_map<std::uintptr_t, std::string>* classes) const noexcept
{
    try {
        const auto object = wordptr(bytes, 0);
        const auto entity_class = wordptr(bytes, 8);
        const auto packed = word32(bytes, 0x10);
        const auto flags = word32(bytes, 0x30);
        const auto handle = DecodeHandle(packed, index);
        if (!object || !entity_class || !handle || (flags & k_invalid_flags) != 0)
            return false;
        std::uintptr_t back_identity{};
        std::uintptr_t class_info{};
        std::uintptr_t back_class{};
        std::uintptr_t cpp_name{};
        if (!read_at(object, 0x10, back_identity) || back_identity != address)
            return false;
        if (classes) {
            const auto found = classes->find(entity_class);
            if (found != classes->end()) {
                out = {*handle, found->second, object};
                return true;
            }
        }
        if (!read_at(entity_class, 0x58, class_info) || !class_info) {
            out = {*handle, {}, object};
            return true;
        }
        if (!read_at(class_info, 0x18, back_class) || back_class != entity_class)
            return false;
        if (!read_at(class_info, 0x8, cpp_name) || !cpp_name) {
            out = {*handle, {}, object};
            return true;
        }
        char name[128]{};
        if (!ReadName(cpp_name, name, sizeof(name))) {
            out = {*handle, {}, object};
            return true;
        }
        out = {*handle, name, object};
        if (classes) classes->emplace(entity_class, out.class_name);
        return true;
    } catch (...) { return false; }
}

bool EntitySystemAdapter::IdentityAt(std::uint32_t index,
                                     LiveIdentity& out) const noexcept
{
    std::uintptr_t system{};
    std::uintptr_t address{};
    std::array<std::byte, k_identity_size> bytes{};
    ReadBatch batch;
    return Current() && System(system) && Slot(system, index, address) &&
           ReadMemory(address, bytes.data(), bytes.size()) &&
           Normalize(address, bytes.data(), index, out);
}

bool EntitySystemAdapter::Resolve(EntityHandle handle, LiveIdentity& out) const noexcept
{
    if (handle.index >= k_max_entities || handle.serial > 0x1ffff)
        return false;
    return IdentityAt(handle.index, out) && out.handle == handle;
}

bool EntitySystemAdapter::Enumerate(std::vector<LiveIdentity>& out,
                                    std::uint32_t& highest_observed,
                                    ReadStats& read_stats) const noexcept
{
    out.clear();
    highest_observed = 0;
    read_stats = {};
    if (!Current()) return false;
    ReadBatch batch;
    std::uintptr_t system{};
    if (!System(system)) return false;
    std::int32_t used_count{};
    std::int32_t dormant_count{};
    if (!read_at(system, 0x228, used_count) ||
        !read_at(system, 0x240, dormant_count) ||
        used_count < 0 || dormant_count < 0 ||
        used_count > static_cast<std::int32_t>(k_max_entities) ||
        dormant_count > static_cast<std::int32_t>(k_max_entities) ||
        used_count + dormant_count > static_cast<std::int32_t>(k_max_entities))
        return false;
    try {
        out.reserve(static_cast<std::size_t>(used_count + dormant_count));
        std::unordered_map<std::uintptr_t, std::string> classes;
        classes.reserve(256);
        std::array<std::byte, k_chunk_size> bytes{};
        for (std::uint32_t chunk_index = 0; chunk_index < 64; ++chunk_index) {
            std::uintptr_t chunk{};
            if (!read_at(system, 0x10 + chunk_index * sizeof(void*), chunk))
                return false;
            if (!chunk) continue;
            if ((chunk & 7) != 0) return false;
            if (!ReadMemory(chunk, bytes.data(), bytes.size())) continue;
            for (std::uint32_t entry = 0; entry < 512; ++entry) {
                const auto index = chunk_index * 512 + entry;
                const auto* raw = bytes.data() + entry * k_identity_size;
                if (!wordptr(raw, 0)) continue;
                std::uintptr_t address{};
                LiveIdentity identity;
                if (!AddAddress(chunk, entry * k_identity_size, address) ||
                    !Normalize(address, raw, index, identity, &classes)) continue;
                highest_observed = index;
                out.push_back(std::move(identity));
                if (out.size() > k_max_entities) return false;
            }
        }
        read_stats = BatchStats();
        return !out.empty() || used_count + dormant_count == 0;
    } catch (...) { out.clear(); return false; }
}

}
