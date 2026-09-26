#include "reflection/schema_service.hpp"

#include "diagnostics.hpp"
#include "module_registry.hpp"
#include "pe_image.hpp"
#include "reflection/interface_resolver.hpp"
#include "source2/abi/schema_abi.hpp"
#include <Windows.h>
#include <Psapi.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace izanagi::schema {
namespace {

std::atomic<State> g_state{State::uninitialized};
std::atomic<bool> g_refresh_requested{false};
std::atomic<bool> g_last_refresh_ok{false};
std::atomic<bool> g_inheritance_probe_ok{false};
std::atomic<Failure> g_last_failure{Failure::none};
std::atomic<std::shared_ptr<const Registry>> g_registry;
std::vector<std::string> g_watched;
struct ModuleIdentity {
    std::uintptr_t base{};
    std::uint32_t timestamp{};
    std::size_t image_size{};
    bool operator==(const ModuleIdentity&) const = default;
};
// estes estados são acessados somente pela thread de trabalho.
std::vector<ModuleIdentity> g_module_bases;
std::uint64_t g_generation = 0;

struct ScopePins {
    std::vector<HMODULE> modules;
    ~ScopePins() noexcept { for (HMODULE m : modules) FreeLibrary(m); }
};

bool valid_module(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 127) return false;
    for (unsigned char c : name) {
        if (c < 0x21 || c > 0x7e || c == '/' || c == '\\' || c == ':') return false;
    }
    return true;
}

bool same_module(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) noexcept {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

std::wstring wide_ascii(std::string_view name)
{
    return std::wstring(name.begin(), name.end());
}

bool modules_present()
{
    if (!modules::Contains(L"schemasystem.dll")) return false;
    for (const auto& name : g_watched) {
        if (!modules::Contains(wide_ascii(name))) return false;
    }
    return true;
}

bool module_bases(std::vector<ModuleIdentity>& bases)
{
    const auto schema = modules::Find(L"schemasystem.dll");
    if (!schema) return false;
    bases.push_back({schema->base, schema->timestamp, schema->image_size});
    for (const auto& name : g_watched) {
        const auto info = modules::Find(wide_ascii(name));
        if (!info) return false;
        bases.push_back({info->base, info->timestamp, info->image_size});
    }
    return true;
}

bool rebuild() noexcept
{
    if (g_state.load(std::memory_order_acquire) == State::shutting_down) return false;
    const auto old = g_registry.load(std::memory_order_acquire);
    const auto started = std::chrono::steady_clock::now();
    try {
        g_state.store(old ? State::refreshing : State::resolving_interface,
                      std::memory_order_release);
        const auto module = modules::Find(L"schemasystem.dll");
        if (!module || !modules_present()) {
            g_last_failure.store(Failure::module_missing, std::memory_order_release);
            g_last_refresh_ok.store(false, std::memory_order_release);
            g_state.store(State::unavailable, std::memory_order_release);
            return false;
        }
        if (!source2::abi::MatchesProfile(module->timestamp, module->image_size)) {
            g_last_failure.store(Failure::profile_mismatch, std::memory_order_release);
            g_last_refresh_ok.store(false, std::memory_order_release);
            g_state.store(State::unavailable, std::memory_order_release);
            return false;
        }
        auto interface = interfaces::Resolve(L"schemasystem.dll", "SchemaSystem_001");
        if (!interface) {
            g_last_failure.store(Failure::interface_unavailable, std::memory_order_release);
            g_last_refresh_ok.store(false, std::memory_order_release);
            g_state.store(old ? State::ready : State::unavailable, std::memory_order_release);
            report_error("SchemaSystem_001 unavailable", ERROR_PROC_NOT_FOUND);
            return false;
        }
        if (!source2::abi::ValidateInterface(interface.get(), interface.base(),
                                              interface.image_size())) {
            g_last_failure.store(Failure::interface_abi_invalid, std::memory_order_release);
            g_last_refresh_ok.store(false, std::memory_order_release);
            g_state.store(old ? State::ready : State::unavailable, std::memory_order_release);
            report_error("SchemaSystem_001 unavailable", ERROR_INVALID_DATA);
            return false;
        }
        const auto interface_ready = std::chrono::steady_clock::now();
        g_state.store(State::discovering_scopes, std::memory_order_release);
        ScopePins pinned_scopes;
        pinned_scopes.modules.reserve(g_watched.size());
        for (const auto& name : g_watched) {
            const auto info = modules::Find(wide_ascii(name));
            HMODULE pinned = nullptr;
            if (!info || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                              reinterpret_cast<LPCWSTR>(info->handle), &pinned)) {
                if (pinned) FreeLibrary(pinned);
                g_last_failure.store(Failure::scope_pin_failed, std::memory_order_release);
                g_last_refresh_ok.store(false, std::memory_order_release);
                g_state.store(old ? State::ready : State::unavailable, std::memory_order_release);
                return false;
            }
            MODULEINFO mapped{};
            const bool queried = GetModuleInformation(GetCurrentProcess(), pinned,
                                                       &mapped, sizeof(mapped)) != 0;
            const auto image = queried ? pe::InspectImage(mapped.lpBaseOfDll,
                                                           mapped.SizeOfImage) : std::nullopt;
            if (!image || image->base != info->base ||
                image->image_size != info->image_size ||
                image->timestamp != info->timestamp) {
                FreeLibrary(pinned);
                g_last_failure.store(Failure::scope_pin_failed, std::memory_order_release);
                g_last_refresh_ok.store(false, std::memory_order_release);
                g_state.store(old ? State::ready : State::unavailable, std::memory_order_release);
                return false;
            }
            pinned_scopes.modules.push_back(pinned);
        }
        const auto pins_ready = std::chrono::steady_clock::now();
        std::vector<ClassInfo> classes;
        bool copied = true;
        for (const auto& name : g_watched) {
            if (!source2::abi::CopyScope(interface.get(), interface.base(),
                                          interface.image_size(), name, classes)) {
                copied = false;
                break;
            }
        }
        if (!copied) {
            g_last_failure.store(Failure::scope_abi_invalid, std::memory_order_release);
            g_last_refresh_ok.store(false, std::memory_order_release);
            g_state.store(old ? State::ready : State::unavailable, std::memory_order_release);
            report_error("schema ABI validation", ERROR_INVALID_DATA);
            return false;
        }
        const auto scope_copied = std::chrono::steady_clock::now();
        g_state.store(State::building_registry, std::memory_order_release);
        auto next = Registry::Build(std::move(classes), g_generation + 1);
        if (!next) {
            g_last_failure.store(Failure::registry_invalid, std::memory_order_release);
            g_last_refresh_ok.store(false, std::memory_order_release);
            g_state.store(old ? State::ready : State::unavailable, std::memory_order_release);
            report_error("schema registry build", ERROR_INVALID_DATA);
            return false;
        }
        const auto registry_built = std::chrono::steady_clock::now();
        const auto probe = next->InheritanceProbe();
        if (!probe) {
            g_last_failure.store(Failure::inheritance_probe_failed, std::memory_order_release);
            g_inheritance_probe_ok.store(false, std::memory_order_release);
            g_last_refresh_ok.store(false, std::memory_order_release);
            g_state.store(old ? State::ready : State::unavailable, std::memory_order_release);
            report_error("schema inheritance probe", ERROR_INVALID_DATA);
            return false;
        }
        const auto probe_finished = std::chrono::steady_clock::now();
        std::vector<ModuleIdentity> bases;
        if (!module_bases(bases)) {
            g_last_failure.store(Failure::module_changed, std::memory_order_release);
            g_last_refresh_ok.store(false, std::memory_order_release);
            g_state.store(old ? State::ready : State::unavailable, std::memory_order_release);
            return false;
        }
        ++g_generation;
        g_module_bases = std::move(bases);
        g_inheritance_probe_ok.store(true, std::memory_order_release);
        g_registry.store(std::move(next), std::memory_order_release);
        g_last_refresh_ok.store(true, std::memory_order_release);
        g_last_failure.store(Failure::none, std::memory_order_release);
        g_state.store(State::ready, std::memory_order_release);
        const auto millis = [](auto from, auto to) noexcept {
            return std::chrono::duration<double, std::milli>(to - from).count();
        };
        (void)std::fprintf(stdout,
                           "izanagi: schema timing interface=%.1f ms pin=%.1f ms "
                           "copy=%.1f ms "
                           "registry=%.1f ms probe=%.1f ms total=%.1f ms\n",
                           millis(started, interface_ready),
                           millis(interface_ready, pins_ready),
                           millis(pins_ready, scope_copied),
                           millis(scope_copied, registry_built),
                           millis(registry_built, probe_finished),
                           millis(started, probe_finished));
        (void)std::fprintf(stdout, "izanagi: inherited field %s::%s -> %s::%s\n",
                           probe->requested_class.c_str(), probe->field.name.c_str(),
                           probe->declaring_class.c_str(), probe->field.name.c_str());
        (void)std::fputs("izanagi: schema registry built\n", stdout);
        return true;
    } catch (...) {
        g_last_failure.store(Failure::exception, std::memory_order_release);
        g_last_refresh_ok.store(false, std::memory_order_release);
        g_state.store(old ? State::ready : State::unavailable, std::memory_order_release);
        report_error("schema refresh exception", ERROR_UNHANDLED_EXCEPTION);
        return false;
    }
}

template <typename T, typename F>
Result<T> query(F&& fn) noexcept
{
    const auto state = g_state.load(std::memory_order_acquire);
    if (state != State::ready && state != State::refreshing &&
        state != State::discovering_scopes && state != State::building_registry)
        return {Status::registry_unavailable, {}};
    try {
        auto registry = g_registry.load(std::memory_order_acquire);
        return registry ? fn(*registry) : Result<T>{Status::registry_unavailable, {}};
    } catch (...) { return {Status::registry_unavailable, {}}; }
}

}

bool WatchScope(std::string_view module) noexcept
{
    if (g_state.load(std::memory_order_acquire) != State::uninitialized ||
        !valid_module(module)) return false;
    try {
        for (const auto& existing : g_watched) if (same_module(existing, module)) return true;
        if (g_watched.size() >= 32) return false;
        g_watched.emplace_back(module);
        return true;
    } catch (...) { return false; }
}

bool Initialize() noexcept
{
    State expected = State::uninitialized;
    if (!g_state.compare_exchange_strong(expected, State::resolving_interface)) return false;
    if (g_watched.empty()) {
        g_last_failure.store(Failure::module_missing, std::memory_order_release);
        g_state.store(State::unavailable, std::memory_order_release);
        return true;
    }
    // adia a cópia do schema até depois da instalação do hook dx11.
    g_refresh_requested.store(true, std::memory_order_release);
    return true;
}

void Poll() noexcept
{
    const auto state = g_state.load(std::memory_order_acquire);
    if ((state == State::resolving_interface || state == State::ready ||
         state == State::unavailable) &&
        g_refresh_requested.exchange(false, std::memory_order_acq_rel)) (void)rebuild();
}

void RequestRefresh() noexcept
{
    const auto state = g_state.load(std::memory_order_acquire);
    if (state == State::ready || state == State::unavailable)
        g_refresh_requested.store(true, std::memory_order_release);
}

void OnModulesUpdated() noexcept
{
    const auto state = g_state.load(std::memory_order_acquire);
    if (state != State::ready && state != State::unavailable) return;
    try {
        std::vector<ModuleIdentity> bases;
        if (!module_bases(bases)) {
            g_last_failure.store(Failure::module_missing, std::memory_order_release);
            g_state.store(State::unavailable, std::memory_order_release);
            g_last_refresh_ok.store(false, std::memory_order_release);
        } else if (state == State::unavailable || bases != g_module_bases) {
            if (bases != g_module_bases) {
                g_last_failure.store(Failure::module_changed, std::memory_order_release);
                g_state.store(State::unavailable, std::memory_order_release);
            }
            RequestRefresh();
        }
    } catch (...) {
        g_last_failure.store(Failure::exception, std::memory_order_release);
        g_state.store(State::unavailable, std::memory_order_release);
    }
}

void BeginShutdown() noexcept
{
    g_state.store(State::shutting_down, std::memory_order_release);
    g_refresh_requested.store(false, std::memory_order_release);
}

void Shutdown() noexcept
{
    BeginShutdown();
    g_registry.store({}, std::memory_order_release);
    g_watched.clear();
    g_module_bases.clear();
    g_state.store(State::stopped, std::memory_order_release);
}

SnapshotData Snapshot() noexcept
{
    SnapshotData result;
    result.state = g_state.load(std::memory_order_acquire);
    result.last_refresh_ok = g_last_refresh_ok.load(std::memory_order_acquire);
    result.inheritance_probe_ok = g_inheritance_probe_ok.load(std::memory_order_acquire);
    result.last_failure = g_last_failure.load(std::memory_order_acquire);
    try {
        auto registry = g_registry.load(std::memory_order_acquire);
        if (registry) {
            result.generation = registry->generation();
            result.scopes = registry->scope_count();
            result.classes = registry->class_count();
            result.fields = registry->field_count();
        }
    } catch (...) {}
    return result;
}

const char* StateName(State state) noexcept
{
    switch (state) {
    case State::uninitialized: return "uninitialized";
    case State::resolving_interface: return "resolving interface";
    case State::discovering_scopes: return "discovering scopes";
    case State::building_registry: return "building registry";
    case State::ready: return "ready";
    case State::refreshing: return "refreshing";
    case State::unavailable: return "unavailable";
    case State::shutting_down: return "shutting down";
    case State::stopped: return "stopped";
    }
    return "unknown";
}

const char* FailureName(Failure failure) noexcept
{
    switch (failure) {
    case Failure::none: return "none";
    case Failure::module_missing: return "module missing";
    case Failure::profile_mismatch: return "ABI profile mismatch";
    case Failure::interface_unavailable: return "interface unavailable";
    case Failure::interface_abi_invalid: return "interface ABI invalid";
    case Failure::scope_pin_failed: return "scope module changed";
    case Failure::scope_abi_invalid: return "scope ABI invalid";
    case Failure::registry_invalid: return "registry invalid";
    case Failure::inheritance_probe_failed: return "inheritance probe failed";
    case Failure::module_changed: return "module changed";
    case Failure::exception: return "exception";
    }
    return "unknown";
}

std::shared_ptr<const Registry> RegistrySnapshot() noexcept
{
    const auto state = g_state.load(std::memory_order_acquire);
    if (state != State::ready && state != State::refreshing &&
        state != State::discovering_scopes && state != State::building_registry) return {};
    try { return g_registry.load(std::memory_order_acquire); }
    catch (...) { return {}; }
}

Result<ScopeInfo> FindScope(std::string_view scope) noexcept
{ return query<ScopeInfo>([&](const Registry& r) { return r.FindScope(scope); }); }
Result<ClassInfo> FindClass(std::string_view scope, std::string_view name) noexcept
{ return query<ClassInfo>([&](const Registry& r) { return r.FindClass(scope, name); }); }
Result<FieldMatch> FindDeclaredField(std::string_view scope, std::string_view name,
                                     std::string_view field) noexcept
{ return query<FieldMatch>([&](const Registry& r) { return r.FindDeclaredField(scope, name, field); }); }
Result<FieldMatch> FindField(std::string_view scope, std::string_view name,
                             std::string_view field) noexcept
{ return query<FieldMatch>([&](const Registry& r) { return r.FindField(scope, name, field); }); }
Result<bool> IsDerivedFrom(std::string_view scope, std::string_view derived,
                            std::string_view base) noexcept
{ return query<bool>([&](const Registry& r) { return r.IsDerivedFrom(scope, derived, base); }); }

}
