#include "entities/field_reader.hpp"

#include "entities/safe_read.hpp"
#include <array>
#include <cstring>
#include <initializer_list>

namespace izanagi::entities::detail {
namespace {

bool equals(std::string_view value, std::initializer_list<std::string_view> names) noexcept
{
    for (const auto name : names) if (value == name) return true;
    return false;
}

FieldStatus from_schema(schema::Status status) noexcept
{
    switch (status) {
    case schema::Status::found: return FieldStatus::found;
    case schema::Status::not_found: return FieldStatus::field_not_found;
    case schema::Status::scope_unavailable:
    case schema::Status::registry_unavailable: return FieldStatus::schema_unavailable;
    case schema::Status::ambiguous: return FieldStatus::ambiguous_field;
    case schema::Status::invalid_query: return FieldStatus::field_not_found;
    }
    return FieldStatus::unavailable;
}

bool supported(std::string_view name) noexcept
{
    constexpr std::array types{
        ValueType::boolean, ValueType::i8, ValueType::u8, ValueType::i16,
        ValueType::u16, ValueType::i32, ValueType::u32, ValueType::i64,
        ValueType::u64, ValueType::f32, ValueType::f64, ValueType::pointer
    };
    for (const auto type : types) if (TypeMatches(name, type)) return true;
    return false;
}

std::size_t type_size(ValueType type) noexcept
{
    switch (type) {
    case ValueType::boolean: case ValueType::i8: case ValueType::u8: return 1;
    case ValueType::i16: case ValueType::u16: return 2;
    case ValueType::i32: case ValueType::u32: case ValueType::f32: return 4;
    case ValueType::i64: case ValueType::u64: case ValueType::f64: return 8;
    case ValueType::pointer: return sizeof(std::uintptr_t);
    }
    return 0;
}

}

bool TypeMatches(std::string_view name, ValueType type) noexcept
{
    switch (type) {
    case ValueType::boolean: return name == "bool";
    case ValueType::i8: return equals(name, {"int8", "int8_t", "signed char"});
    case ValueType::u8: return equals(name, {"uint8", "uint8_t", "unsigned char", "char"});
    case ValueType::i16: return equals(name, {"int16", "int16_t", "short"});
    case ValueType::u16: return equals(name, {"uint16", "uint16_t", "unsigned short"});
    case ValueType::i32: return equals(name, {"int32", "int32_t", "int"});
    case ValueType::u32: return equals(name, {"uint32", "uint32_t", "unsigned int"});
    case ValueType::i64: return equals(name, {"int64", "int64_t", "long long"});
    case ValueType::u64: return equals(name, {"uint64", "uint64_t", "unsigned long long"});
    case ValueType::f32: return equals(name, {"float32", "float"});
    case ValueType::f64: return equals(name, {"float64", "double"});
    case ValueType::pointer: return !name.empty() && name.back() == '*';
    }
    return false;
}

FieldStatus ReadField(const schema::Registry& registry,
                      std::string_view scope, std::string_view class_name,
                      std::string_view field_name, std::uintptr_t object,
                      ValueType type, void* target, std::size_t size) noexcept
{
    if (!target || size == 0 || size > sizeof(std::uintptr_t))
        return FieldStatus::unsupported_type;
    if (size != type_size(type)) return FieldStatus::type_mismatch;
    try {
        const auto cls = registry.FindClass(scope, class_name);
        if (!cls) return FieldStatus::class_unavailable;
        const auto field = registry.FindField(scope, class_name, field_name);
        if (!field) return from_schema(field.status);
        if (!supported(field.value.field.type_name))
            return FieldStatus::unsupported_type;
        if (!TypeMatches(field.value.field.type_name, type))
            return FieldStatus::type_mismatch;
        if (field.value.effective_offset > cls.value.size ||
            size > cls.value.size - field.value.effective_offset)
            return FieldStatus::invalid_address;
        std::uintptr_t address{};
        if (!AddAddress(object, field.value.effective_offset, address))
            return FieldStatus::invalid_address;
        std::array<std::byte, sizeof(std::uintptr_t)> bytes{};
        if (!ReadMemory(address, bytes.data(), size)) return FieldStatus::read_failed;
        if (type == ValueType::boolean) {
            const bool value = bytes[0] != std::byte{};
            std::memcpy(target, &value, sizeof(value));
        } else {
            std::memcpy(target, bytes.data(), size);
        }
        return FieldStatus::found;
    } catch (...) { return FieldStatus::read_failed; }
}

const char* FieldStatusName(FieldStatus status) noexcept
{
    switch (status) {
    case FieldStatus::found: return "found";
    case FieldStatus::entity_invalid: return "entity invalid";
    case FieldStatus::schema_unavailable: return "schema unavailable";
    case FieldStatus::class_unavailable: return "class unavailable";
    case FieldStatus::field_not_found: return "field not found";
    case FieldStatus::type_mismatch: return "type mismatch";
    case FieldStatus::unsupported_type: return "unsupported type";
    case FieldStatus::invalid_address: return "invalid address";
    case FieldStatus::read_failed: return "read failed";
    case FieldStatus::ambiguous_field: return "ambiguous field";
    case FieldStatus::unavailable: return "unavailable";
    }
    return "unknown";
}

}
