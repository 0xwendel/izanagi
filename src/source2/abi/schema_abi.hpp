#pragma once

#include "reflection/schema_registry.hpp"
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace izanagi::source2::abi {

// perfil da build inspecionada; revisar a abi antes de usar outra versão da dll.
bool MatchesProfile(std::uint32_t timestamp, std::size_t image_size) noexcept;
bool ValidateInterface(void* system, std::uintptr_t image_base,
                       std::size_t image_size) noexcept;
bool CopyScope(void* system, std::uintptr_t image_base, std::size_t image_size,
               std::string_view module, std::vector<schema::ClassInfo>& out) noexcept;

}
