#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace izanagi::modules {

// Metadata is a point-in-time copy. handle/base may become stale after Refresh.
struct ModuleInfo {
    std::wstring name;
    HMODULE handle{};
    std::uintptr_t base{};
    std::size_t image_size{};
    std::uint32_t entry_point_rva{};
    std::uint32_t timestamp{};
    std::uint16_t sections{};
};

struct SnapshotData {
    std::size_t loaded_count{};
    std::size_t invalid_pe_count{};
    DWORD last_error{};
    std::uint64_t generation{};
};

bool Initialize() noexcept;
bool Refresh() noexcept;
void Shutdown() noexcept;
std::optional<ModuleInfo> Find(std::wstring_view name) noexcept;
bool Contains(std::wstring_view name) noexcept;
SnapshotData Snapshot() noexcept;

}
