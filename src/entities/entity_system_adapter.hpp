#pragma once

#include "entities/entity_types.hpp"
#include "entities/safe_read.hpp"
#include "module_registry.hpp"
#include "reflection/interface_resolver.hpp"
#include <Windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace izanagi::entities::detail {

struct LiveIdentity {
    EntityHandle handle;
    std::string class_name;
    std::uintptr_t object{};
};

class EntitySystemAdapter final {
public:
    static std::unique_ptr<EntitySystemAdapter> Acquire(Failure& failure) noexcept;
    ~EntitySystemAdapter() noexcept;
    EntitySystemAdapter(const EntitySystemAdapter&) = delete;
    EntitySystemAdapter& operator=(const EntitySystemAdapter&) = delete;

    bool Current() const noexcept;
    bool Available() const noexcept;
    bool IdentityAt(std::uint32_t index, LiveIdentity& out) const noexcept;
    bool Resolve(EntityHandle handle, LiveIdentity& out) const noexcept;
    bool Enumerate(std::vector<LiveIdentity>& out,
                   std::uint32_t& highest_observed,
                   ReadStats& read_stats) const noexcept;

private:
    EntitySystemAdapter(interfaces::Lease engine, HMODULE client,
                        modules::ModuleInfo engine_info,
                        modules::ModuleInfo client_info) noexcept;
    bool System(std::uintptr_t& result) const noexcept;
    bool Slot(std::uintptr_t system, std::uint32_t index,
              std::uintptr_t& result) const noexcept;
    bool Normalize(std::uintptr_t address, const std::byte* bytes,
                   std::uint32_t index, LiveIdentity& out,
                   std::unordered_map<std::uintptr_t, std::string>* classes = nullptr) const noexcept;

    interfaces::Lease engine_;
    HMODULE client_{};
    modules::ModuleInfo engine_info_;
    modules::ModuleInfo client_info_;
};

}
