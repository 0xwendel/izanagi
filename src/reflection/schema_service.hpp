#pragma once

#include "reflection/schema_registry.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace izanagi::schema {

enum class State : std::uint8_t {
    uninitialized, resolving_interface, discovering_scopes, building_registry,
    ready, refreshing, unavailable, shutting_down, stopped
};

enum class Failure : std::uint8_t {
    none, module_missing, profile_mismatch, interface_unavailable,
    interface_abi_invalid, scope_pin_failed, scope_abi_invalid,
    registry_invalid, inheritance_probe_failed, module_changed, exception
};

struct SnapshotData {
    State state{State::uninitialized};
    std::uint64_t generation{};
    std::size_t scopes{};
    std::size_t classes{};
    std::size_t fields{};
    bool last_refresh_ok{};
    bool inheritance_probe_ok{};
    Failure last_failure{Failure::none};
};

// configurar na thread de trabalho antes da inicialização; usar nomes base dos módulos.
bool WatchScope(std::string_view module) noexcept;
bool Initialize() noexcept;
void Poll() noexcept; // consome pedidos na thread de trabalho, fora de present
void RequestRefresh() noexcept;
void OnModulesUpdated() noexcept; // chamar após atualizar os módulos na thread de trabalho
void BeginShutdown() noexcept;
void Shutdown() noexcept; // chamar após drenar os callbacks externos
SnapshotData Snapshot() noexcept;
const char* StateName(State state) noexcept;
const char* FailureName(Failure failure) noexcept;

// snapshot imutável sem ponteiros da engine; não reter após descarregar a dll.
std::shared_ptr<const Registry> RegistrySnapshot() noexcept;
Result<ScopeInfo> FindScope(std::string_view scope) noexcept;
Result<ClassInfo> FindClass(std::string_view scope, std::string_view name) noexcept;
Result<FieldMatch> FindDeclaredField(std::string_view scope, std::string_view name,
                                     std::string_view field) noexcept;
Result<FieldMatch> FindField(std::string_view scope, std::string_view name,
                             std::string_view field) noexcept;
Result<bool> IsDerivedFrom(std::string_view scope, std::string_view derived,
                            std::string_view base) noexcept;

}
