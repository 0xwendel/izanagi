#include "entities/entity_service.hpp"

#include "entities/entity_system_adapter.hpp"
#include "entities/field_reader.hpp"
#include "reflection/schema_service.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace izanagi::entities {
namespace {

using Clock = std::chrono::steady_clock;
std::atomic<State> g_state{State::uninitialized};
std::atomic<bool> g_stopping{false};
std::atomic<Failure> g_failure{Failure::none};
std::atomic<double> g_last_tick_ms{};
std::atomic<std::shared_ptr<const EntityFrameSnapshot>> g_snapshot;
std::mutex g_lock;
std::unique_ptr<detail::EntitySystemAdapter> g_adapter;
Clock::time_point g_next_attempt{};
Clock::time_point g_next_scan{};
std::atomic<std::uint64_t> g_frame_index{};
std::uint64_t g_generation{};

bool running() noexcept
{
    return !g_stopping.load(std::memory_order_acquire) &&
           g_state.load(std::memory_order_acquire) == State::ready;
}

}

bool Initialize() noexcept
{
    State expected = State::uninitialized;
    if (!g_state.compare_exchange_strong(expected, State::resolving)) return false;
    g_stopping.store(false, std::memory_order_release);
    g_snapshot.store({}, std::memory_order_release);
    g_failure.store(Failure::none, std::memory_order_release);
    g_next_attempt = Clock::time_point{};
    g_next_scan = Clock::time_point{};
    g_frame_index.store(0, std::memory_order_release);
    g_last_tick_ms.store(0, std::memory_order_release);
    return true;
}

void Poll() noexcept
{
    const auto state = g_state.load(std::memory_order_acquire);
    if (state != State::resolving && state != State::unavailable &&
        state != State::ready) return;
    const auto now = Clock::now();
    std::lock_guard lock(g_lock);
    if (g_stopping.load(std::memory_order_acquire)) return;
    const auto locked_state = g_state.load(std::memory_order_acquire);
    if (locked_state == State::ready && now < g_next_scan) return;
    if (locked_state != State::ready &&
        locked_state != State::resolving && locked_state != State::unavailable) return;
    if (locked_state != State::ready && now < g_next_attempt) return;
    try {
        if (locked_state != State::ready) {
            g_next_attempt = now + std::chrono::seconds(1);
            if (!schema::RegistrySnapshot()) {
                g_failure.store(Failure::schema_unavailable, std::memory_order_release);
                g_state.store(State::unavailable, std::memory_order_release);
                return;
            }
            Failure failure{};
            g_adapter = detail::EntitySystemAdapter::Acquire(failure);
            g_failure.store(failure, std::memory_order_release);
            if (!g_adapter) {
                g_state.store(State::unavailable, std::memory_order_release);
                return;
            }
            g_state.store(State::ready, std::memory_order_release);
            g_next_scan = Clock::now();
            (void)std::fputs("izanagi: entity service ready\n", stdout);
        }
        if (!g_adapter || !g_adapter->Current()) {
            g_failure.store(Failure::module_changed, std::memory_order_release);
            g_state.store(State::unavailable, std::memory_order_release);
            g_adapter.reset();
            g_snapshot.store({}, std::memory_order_release);
            return;
        }
        std::vector<detail::LiveIdentity> live;
        std::uint32_t highest{};
        detail::ReadStats read_stats;
        if (!g_adapter->Enumerate(live, highest, read_stats)) {
            g_failure.store(Failure::enumeration_invalid, std::memory_order_release);
            g_state.store(State::unavailable, std::memory_order_release);
            g_adapter.reset();
            g_snapshot.store({}, std::memory_order_release);
            return;
        }
        auto registry = schema::RegistrySnapshot();
        auto next = std::make_shared<EntityFrameSnapshot>();
        next->frame_index = g_frame_index.load(std::memory_order_acquire);
        next->schema_generation = registry ? registry->generation() : 0;
        next->highest_observed_index = highest;
        next->virtual_queries = read_stats.queries;
        next->query_cache_hits = read_stats.cache_hits;
        next->virtual_query_ms = read_stats.query_ms;
        next->entities.reserve(live.size());
        std::unordered_map<std::string, bool> known_classes;
        for (auto& item : live) {
            bool known = false;
            if (registry && !item.class_name.empty()) {
                const auto found = known_classes.find(item.class_name);
                if (found != known_classes.end()) {
                    known = found->second;
                } else {
                    known = static_cast<bool>(registry->FindClass("client.dll",
                                                                  item.class_name));
                    known_classes.emplace(item.class_name, known);
                }
            }
            next->entities.push_back({{item.handle, "client.dll",
                                       std::move(item.class_name), known}});
        }
        next->generation = ++g_generation;
        next->scan_ms = std::chrono::duration<double, std::milli>(Clock::now() - now).count();
        g_snapshot.store(std::move(next), std::memory_order_release);
        g_failure.store(Failure::none, std::memory_order_release);
        g_next_scan = Clock::now() + std::chrono::seconds(1);
    } catch (...) {
        g_adapter.reset();
        g_failure.store(Failure::exception, std::memory_order_release);
        g_state.store(State::unavailable, std::memory_order_release);
        g_snapshot.store({}, std::memory_order_release);
    }
}

void OnModulesUpdated() noexcept
{
    std::lock_guard lock(g_lock);
    if (running() && g_adapter && !g_adapter->Current()) {
        g_failure.store(Failure::module_changed, std::memory_order_release);
        g_state.store(State::unavailable, std::memory_order_release);
        g_adapter.reset();
        g_snapshot.store({}, std::memory_order_release);
    }
}

void Tick(const FrameContext& frame) noexcept
{
    if (!running()) return;
    const auto start = Clock::now();
    g_frame_index.store(frame.frame_index, std::memory_order_release);
    g_last_tick_ms.store(
        std::chrono::duration<double, std::milli>(Clock::now() - start).count(),
        std::memory_order_release);
}

void BeginShutdown() noexcept
{
    g_stopping.store(true, std::memory_order_release);
    std::lock_guard lock(g_lock);
    const auto state = g_state.load(std::memory_order_acquire);
    if (state != State::stopped && state != State::uninitialized)
        g_state.store(State::shutting_down, std::memory_order_release);
}

void Shutdown() noexcept
{
    BeginShutdown();
    std::lock_guard lock(g_lock);
    g_snapshot.store({}, std::memory_order_release);
    g_adapter.reset();
    g_state.store(State::stopped, std::memory_order_release);
}

State CurrentState() noexcept { return g_state.load(std::memory_order_acquire); }
Failure LastFailure() noexcept { return g_failure.load(std::memory_order_acquire); }
double LastTickMs() noexcept { return g_last_tick_ms.load(std::memory_order_acquire); }

const char* StateName(State state) noexcept
{
    switch (state) {
    case State::uninitialized: return "uninitialized";
    case State::resolving: return "resolving";
    case State::ready: return "ready";
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
    case Failure::schema_unavailable: return "schema unavailable";
    case Failure::module_missing: return "module missing";
    case Failure::profile_mismatch: return "ABI profile mismatch";
    case Failure::interface_unavailable: return "interface unavailable";
    case Failure::system_unavailable: return "entity system unavailable/ABI invalid";
    case Failure::enumeration_invalid: return "entity enumeration invalid";
    case Failure::module_changed: return "module changed";
    case Failure::exception: return "exception";
    }
    return "unknown";
}

std::shared_ptr<const EntityFrameSnapshot> Snapshot() noexcept
{
    try { return g_snapshot.load(std::memory_order_acquire); }
    catch (...) { return {}; }
}

std::optional<EntityView> Resolve(EntityHandle handle) noexcept
{
    if (!running()) return std::nullopt;
    std::lock_guard lock(g_lock);
    if (!running() || !g_adapter) return std::nullopt;
    try {
        detail::LiveIdentity live;
        if (!g_adapter->Resolve(handle, live)) return std::nullopt;
        const auto registry = schema::RegistrySnapshot();
        return EntityView(handle, std::move(live.class_name),
                          registry ? registry->generation() : 0);
    } catch (...) { return std::nullopt; }
}

FieldStatus ReadField(EntityHandle handle, std::string_view expected_class,
                      std::uint64_t schema_generation, std::string_view field_name,
                      ValueType type, void* target, std::size_t size) noexcept
{
    if (!running()) return FieldStatus::unavailable;
    std::lock_guard lock(g_lock);
    if (!running() || !g_adapter) return FieldStatus::unavailable;
    try {
        const auto registry = schema::RegistrySnapshot();
        if (!registry || registry->generation() != schema_generation)
            return FieldStatus::schema_unavailable;
        detail::LiveIdentity live;
        if (!g_adapter->Resolve(handle, live) || live.class_name != expected_class)
            return FieldStatus::entity_invalid;
        const auto status = detail::ReadField(*registry, "client.dll", live.class_name,
                                               field_name, live.object, type,
                                               target, size);
        if (status != FieldStatus::found) return status;
        detail::LiveIdentity after;
        if (!g_adapter->Resolve(handle, after) || after.object != live.object ||
            after.class_name != live.class_name) {
            if (target) std::memset(target, 0, size);
            return FieldStatus::entity_invalid;
        }
        return status;
    } catch (...) { return FieldStatus::read_failed; }
}

bool IsA(EntityHandle handle, std::string_view expected_class,
         std::uint64_t schema_generation, std::string_view base_class) noexcept
{
    if (!running()) return false;
    std::lock_guard lock(g_lock);
    if (!running() || !g_adapter) return false;
    try {
        const auto registry = schema::RegistrySnapshot();
        if (!registry || registry->generation() != schema_generation) return false;
        detail::LiveIdentity live;
        if (!g_adapter->Resolve(handle, live) || live.class_name != expected_class ||
            !registry->FindClass("client.dll", live.class_name)) return false;
        if (live.class_name == base_class) return true;
        const auto derived = registry->IsDerivedFrom("client.dll", live.class_name,
                                                     base_class);
        return derived && derived.value;
    } catch (...) { return false; }
}

}
