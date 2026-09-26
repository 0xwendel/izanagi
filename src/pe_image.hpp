#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace izanagi::pe {

struct Metadata {
    std::uintptr_t base{};
    std::size_t image_size{};
    std::uint32_t entry_point_rva{};
    std::uint32_t timestamp{};
    std::uint16_t sections{};
};

// Examines a mapped image, not a raw PE file. Reads only committed, readable pages.
std::optional<Metadata> InspectImage(const void* base, std::size_t mapped_size) noexcept;

}
