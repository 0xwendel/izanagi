#include "spatial/spatial_service.hpp"

#include "entities/entity_service.hpp"
#include "entities/safe_read.hpp"
#include "reflection/schema_service.hpp"
#include "source2/abi/spatial.hpp"
#include "spatial/camera_service.hpp"
#include "spatial/projection.hpp"
#include <atomic>
#include <chrono>
#include <mutex>

namespace izanagi::spatial {
namespace {

using Clock = std::chrono::steady_clock;
std::atomic<ServiceState> g_state{ServiceState::uninitialized};
std::atomic<std::shared_ptr<const SpatialFrameSnapshot>> g_snapshot;
std::mutex g_lock;
source2::abi::TransformResolver g_resolver;
std::uint64_t g_last_entity_generation{};
std::uint64_t g_last_camera_generation{};

}

bool Initialize() noexcept
{
    ServiceState expected = ServiceState::uninitialized;
    if (!g_state.compare_exchange_strong(expected, ServiceState::resolving)) return false;
    g_snapshot.store({}, std::memory_order_release);
    g_last_entity_generation = 0;
    g_last_camera_generation = 0;
    return true;
}

void Poll() noexcept
{
    const auto state = g_state.load(std::memory_order_acquire);
    if (state != ServiceState::resolving && state != ServiceState::ready &&
        state != ServiceState::unavailable) return;
    std::lock_guard lock(g_lock);
    if (g_state.load(std::memory_order_acquire) == ServiceState::shutting_down) return;
    try {
        const auto source = entities::Snapshot();
        const auto registry = schema::RegistrySnapshot();
        if (!source || !registry || source->schema_generation != registry->generation()) {
            auto empty = std::make_shared<SpatialFrameSnapshot>();
            empty->camera = camera::Snapshot();
            empty->frame_index = source ? source->frame_index : empty->camera.frame_index;
            empty->entity_generation = source ? source->generation : 0;
            empty->schema_generation = registry ? registry->generation() : 0;
            g_snapshot.store(std::move(empty), std::memory_order_release);
            g_state.store(ServiceState::unavailable, std::memory_order_release);
            g_last_entity_generation = 0;
            g_last_camera_generation = 0;
            g_resolver.Reset();
            return;
        }
        const auto camera_snapshot = camera::Snapshot();
        if (source->generation == g_last_entity_generation &&
            camera_snapshot.generation == g_last_camera_generation) return;
        const auto start = Clock::now();
        auto next = std::make_shared<SpatialFrameSnapshot>();
        next->frame_index = source->frame_index;
        next->entity_generation = source->generation;
        next->schema_generation = registry->generation();
        next->camera = camera_snapshot;
        next->entities.reserve(source->entities.size());
        entities::detail::ReadBatch batch;
        for (const auto& entry : source->entities) {
            SpatialEntitySnapshot item;
            item.handle = entry.identity.handle;
            item.class_name = entry.identity.class_name;
            const auto view = entities::Resolve(item.handle);
            if (!view) {
                item.transform_status = Status::entity_invalid;
            } else {
                const auto transform = g_resolver.Resolve(*view, *registry);
                item.transform_status = transform.status;
                if (transform) {
                    item.transform = transform.value;
                    ++next->valid_transforms;
                    item.projection = Project(transform.value.position, next->camera);
                    switch (item.projection.status) {
                    case ProjectionStatus::visible: ++next->projected; break;
                    case ProjectionStatus::outside_viewport: ++next->offscreen; break;
                    case ProjectionStatus::behind_camera: ++next->behind_camera; break;
                    default: break;
                    }
                }
            }
            next->entities.push_back(std::move(item));
        }
        next->tick_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        g_snapshot.store(std::move(next), std::memory_order_release);
        g_last_entity_generation = source->generation;
        g_last_camera_generation = camera_snapshot.generation;
        g_state.store(ServiceState::ready, std::memory_order_release);
    } catch (...) {
        auto empty = std::make_shared<SpatialFrameSnapshot>();
        empty->camera = camera::Snapshot();
        empty->frame_index = empty->camera.frame_index;
        g_snapshot.store(std::move(empty), std::memory_order_release);
        g_state.store(ServiceState::unavailable, std::memory_order_release);
        g_last_entity_generation = 0;
        g_last_camera_generation = 0;
        g_resolver.Reset();
    }
}

void BeginShutdown() noexcept
{
    std::lock_guard lock(g_lock);
    const auto state = g_state.load(std::memory_order_acquire);
    if (state != ServiceState::stopped && state != ServiceState::uninitialized)
        g_state.store(ServiceState::shutting_down, std::memory_order_release);
}

void Shutdown() noexcept
{
    BeginShutdown();
    std::lock_guard lock(g_lock);
    g_snapshot.store({}, std::memory_order_release);
    g_resolver.Reset();
    g_last_entity_generation = 0;
    g_last_camera_generation = 0;
    g_state.store(ServiceState::stopped, std::memory_order_release);
}

ServiceState CurrentState() noexcept
{ return g_state.load(std::memory_order_acquire); }

std::shared_ptr<const SpatialFrameSnapshot> Snapshot() noexcept
{
    try { return g_snapshot.load(std::memory_order_acquire); }
    catch (...) { return {}; }
}

const char* ServiceStateName(ServiceState state) noexcept
{
    switch (state) {
    case ServiceState::uninitialized: return "uninitialized";
    case ServiceState::resolving: return "resolving";
    case ServiceState::ready: return "ready";
    case ServiceState::unavailable: return "unavailable";
    case ServiceState::shutting_down: return "shutting down";
    case ServiceState::stopped: return "stopped";
    }
    return "unknown";
}

const char* StatusName(Status status) noexcept
{
    switch (status) {
    case Status::success: return "success";
    case Status::entity_invalid: return "entity invalid";
    case Status::schema_unavailable: return "schema unavailable";
    case Status::transform_unavailable: return "transform unavailable";
    case Status::field_unavailable: return "field unavailable";
    case Status::type_mismatch: return "type mismatch";
    case Status::invalid_pointer: return "invalid pointer";
    case Status::invalid_value: return "invalid value";
    case Status::unsupported_layout: return "unsupported layout";
    }
    return "unknown";
}

}
