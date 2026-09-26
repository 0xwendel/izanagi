#pragma once

#include "frame_context.hpp"
#include <Windows.h>
#include <cstddef>
#include <cstdint>

namespace izanagi::runtime_services {

enum class State : std::uint8_t {
    uninitialized, initializing, running, shutting_down, stopped, failed
};

struct SnapshotData {
    State state{};
    std::uint64_t frame_index{};
    double frame_time_ms{};
    DWORD pid{};
    bool window_valid{};
    std::size_t loaded_module_count{};
    std::size_t invalid_pe_count{};
    DWORD module_error{};
    std::uint64_t module_generation{};
};

bool Initialize() noexcept;
void Tick(const FrameContext& frame) noexcept;
void RefreshModules() noexcept; // WorkerThread only, outside Present
void BeginShutdown() noexcept;
void Shutdown() noexcept; // after DXGI callback drain
SnapshotData Snapshot() noexcept;
const char* StateName(State state) noexcept;

}
