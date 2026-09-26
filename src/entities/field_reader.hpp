#pragma once

#include "entities/entity_types.hpp"
#include "reflection/schema_registry.hpp"
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace izanagi::entities::detail {

FieldStatus ReadField(const schema::Registry& registry,
                      std::string_view scope, std::string_view class_name,
                      std::string_view field_name, std::uintptr_t object,
                      ValueType type, void* target, std::size_t size) noexcept;
FieldStatus ReadBoundMemory(std::uintptr_t object, const FieldBinding& binding,
                            void* target, std::size_t size) noexcept;
bool TypeMatches(std::string_view schema_type, ValueType type) noexcept;
const char* FieldStatusName(FieldStatus status) noexcept;

}
