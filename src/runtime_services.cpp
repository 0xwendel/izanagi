#include "runtime_services.hpp"

#include "diagnostics.hpp"
#include "module_registry.hpp"
#include <atomic>
#include <chrono>
#include <mutex>

namespace izanagi::runtime_services {
namespace {

std::atomic<State> g_state{State::uninitialized};
std::mutex g_snapshot_lock;
SnapshotData g_snapshot;
bool g_modules_started = false;

}

bool Initialize() noexcept
{
    State expected = State::uninitialized;
    if (!g_state.compare_exchange_strong(expected, State::initializing)) return false;
    try {
        {
            std::lock_guard lock(g_snapshot_lock);
            g_snapshot = {};
            g_snapshot.state = State::initializing;
            g_snapshot.pid = GetCurrentProcessId();
        }
        // The registry is secondary. Enumeration failure degrades telemetry only.
        g_modules_started = modules::Initialize();
        if (g_modules_started && !modules::Refresh()) {
            report_error("modules::Refresh(initial)", modules::Snapshot().last_error);
        }
        g_state.store(State::running, std::memory_order_release);
        return true;
    } catch (...) {
        if (g_modules_started) modules::Shutdown();
        g_modules_started = false;
        g_state.store(State::failed, std::memory_order_release);
        return false;
    }
}

void Tick(const FrameContext& frame) noexcept
{
    if (g_state.load(std::memory_order_acquire) != State::running) return;
    try {
        const auto module = modules::Snapshot();
        std::lock_guard lock(g_snapshot_lock);
        if (g_state.load(std::memory_order_acquire) != State::running) return;
        g_snapshot.state = State::running;
        g_snapshot.frame_index = frame.frame_index;
        g_snapshot.frame_time_ms =
            std::chrono::duration<double, std::milli>(frame.delta_time).count();
        g_snapshot.window_valid = frame.window != nullptr && IsWindow(frame.window);
        g_snapshot.loaded_module_count = module.loaded_count;
        g_snapshot.invalid_pe_count = module.invalid_pe_count;
        g_snapshot.module_error = module.last_error;
        g_snapshot.module_generation = module.generation;
    } catch (...) {
        report_error("runtime_services::Tick", ERROR_UNHANDLED_EXCEPTION);
    }
}

void RefreshModules() noexcept
{
    if (g_state.load(std::memory_order_acquire) == State::running &&
        g_modules_started && !modules::Refresh()) {
        report_error("modules::Refresh", modules::Snapshot().last_error);
    }
}

void BeginShutdown() noexcept
{
    State expected = State::running;
    (void)g_state.compare_exchange_strong(expected, State::shutting_down,
                                          std::memory_order_acq_rel);
}

void Shutdown() noexcept
{
    BeginShutdown();
    // The caller must have disabled hooks and drained all Present callbacks.
    if (g_modules_started) {
        modules::Shutdown();
        g_modules_started = false;
    }
    {
        std::lock_guard lock(g_snapshot_lock);
        g_snapshot = {};
        g_snapshot.state = State::stopped;
    }
    g_state.store(State::stopped, std::memory_order_release);
}

SnapshotData Snapshot() noexcept
{
    std::lock_guard lock(g_snapshot_lock);
    SnapshotData copy = g_snapshot;
    copy.state = g_state.load(std::memory_order_acquire);
    return copy;
}

const char* StateName(State state) noexcept
{
    switch (state) {
    case State::uninitialized: return "uninitialized";
    case State::initializing: return "initializing";
    case State::running: return "running";
    case State::shutting_down: return "shutting down";
    case State::stopped: return "stopped";
    case State::failed: return "failed";
    }
    return "unknown";
}

}
