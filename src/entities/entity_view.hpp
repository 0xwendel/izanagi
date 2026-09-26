#pragma once

#include "entities/entity_types.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace izanagi::entities {

FieldStatus ReadField(EntityHandle handle, std::string_view expected_class,
                      std::uint64_t schema_generation, std::string_view field_name,
                      ValueType type, void* target, std::size_t size) noexcept;
bool IsA(EntityHandle handle, std::string_view expected_class,
         std::uint64_t schema_generation, std::string_view base_class) noexcept;

class EntityView final {
public:
    EntityView(EntityHandle handle, std::string class_name,
               std::uint64_t schema_generation) noexcept
        : handle_(handle), class_name_(std::move(class_name)),
          schema_generation_(schema_generation) {}

    EntityHandle handle() const noexcept { return handle_; }
    std::string_view class_name() const noexcept { return class_name_; }
    std::uint64_t schema_generation() const noexcept { return schema_generation_; }
    bool is_a(std::string_view base_class) const noexcept
    { return IsA(handle_, class_name_, schema_generation_, base_class); }

    template <ReadableValue T>
    FieldResult<T> read(std::string_view field_name) const noexcept
    {
        FieldResult<T> result;
        result.status = ReadField(handle_, class_name_, schema_generation_,
                                  field_name, ValueTypeOf<T>::value,
                                  &result.value, sizeof(T));
        return result;
    }

private:
    EntityHandle handle_;
    std::string class_name_;
    std::uint64_t schema_generation_{};
};

}
