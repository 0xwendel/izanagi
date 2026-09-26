#pragma once

#include "frame_context.hpp"
#include "reflection/schema_service.hpp"
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
    schema::SnapshotData schema{};
};

bool Initialize() noexcept;
void Tick(const FrameContext& frame) noexcept;
void RefreshModules() noexcept; // somente na thread de trabalho, fora de present
void PollServices() noexcept; // somente na thread de trabalho
void BeginShutdown() noexcept;
void Shutdown() noexcept; // após drenar os callbacks dxgi
SnapshotData Snapshot() noexcept;
const char* StateName(State state) noexcept;

}
