#include "spatial/camera_service.hpp"

#include <atomic>

namespace izanagi::spatial::camera {
namespace {

std::atomic<ServiceState> g_state{ServiceState::uninitialized};
std::atomic<std::uint64_t> g_frame{};
std::atomic<std::uint64_t> g_viewport{};
std::atomic<std::uint64_t> g_generation{};

}

bool Initialize() noexcept
{
    ServiceState expected = ServiceState::uninitialized;
    if (!g_state.compare_exchange_strong(expected, ServiceState::resolving)) return false;
    g_frame.store(0, std::memory_order_release);
    g_viewport.store(0, std::memory_order_release);
    g_generation.store(0, std::memory_order_release);
    g_state.store(ServiceState::unavailable, std::memory_order_release);
    return true;
}

void Tick(const FrameContext& frame) noexcept
{
    if (g_state.load(std::memory_order_acquire) != ServiceState::unavailable) return;
    g_frame.store(frame.frame_index, std::memory_order_release);
    const std::uint64_t viewport = frame.backbuffer_width && frame.backbuffer_height &&
        frame.backbuffer_width <= 16384 && frame.backbuffer_height <= 16384
        ? (static_cast<std::uint64_t>(frame.backbuffer_width) << 32) |
          frame.backbuffer_height : 0;
    if (g_viewport.exchange(viewport, std::memory_order_acq_rel) != viewport)
        g_generation.fetch_add(1, std::memory_order_acq_rel);
}

CameraSnapshot Snapshot() noexcept
{
    CameraSnapshot result;
    const auto viewport = g_viewport.load(std::memory_order_acquire);
    result.viewport_width = static_cast<std::uint32_t>(viewport >> 32);
    result.viewport_height = static_cast<std::uint32_t>(viewport);
    result.frame_index = g_frame.load(std::memory_order_acquire);
    result.generation = g_generation.load(std::memory_order_acquire);
    return result;
}

ServiceState CurrentState() noexcept
{ return g_state.load(std::memory_order_acquire); }

void BeginShutdown() noexcept
{
    const auto state = g_state.load(std::memory_order_acquire);
    if (state != ServiceState::stopped && state != ServiceState::uninitialized)
        g_state.store(ServiceState::shutting_down, std::memory_order_release);
}

void Shutdown() noexcept
{
    BeginShutdown();
    g_viewport.store(0, std::memory_order_release);
    g_state.store(ServiceState::stopped, std::memory_order_release);
}

}
