#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace izanagi::entities {

struct EntityHandle {
    std::uint32_t index{};
    std::uint32_t serial{};
    bool operator==(const EntityHandle&) const = default;
};

constexpr std::optional<EntityHandle> DecodeHandle(std::uint32_t packed,
                                                   std::uint32_t slot) noexcept
{
    if (slot >= 32768 || packed == 0xffffffff || (packed & 0x7fff) != slot)
        return std::nullopt;
    return EntityHandle{slot, packed >> 15};
}

struct EntityIdentity {
    EntityHandle handle;
    std::string scope;
    std::string class_name;
    bool schema_valid{};
};

struct EntitySnapshot {
    EntityIdentity identity;
};

struct EntityFrameSnapshot {
    std::uint64_t generation{};
    std::uint64_t frame_index{};
    std::uint64_t schema_generation{};
    std::uint32_t highest_observed_index{};
    double scan_ms{};
    std::uint64_t virtual_queries{};
    std::uint64_t query_cache_hits{};
    double virtual_query_ms{};
    std::vector<EntitySnapshot> entities;
};

enum class State : std::uint8_t {
    uninitialized, resolving, ready, unavailable, shutting_down, stopped
};

enum class Failure : std::uint8_t {
    none, schema_unavailable, module_missing, profile_mismatch,
    interface_unavailable, system_unavailable, enumeration_invalid,
    module_changed, exception
};

enum class FieldStatus : std::uint8_t {
    found, entity_invalid, schema_unavailable, class_unavailable,
    field_not_found, type_mismatch, unsupported_type, invalid_address,
    read_failed, ambiguous_field, unavailable
};

enum class ValueType : std::uint8_t {
    boolean, i8, u8, i16, u16, i32, u32, i64, u64, f32, f64, pointer
};

struct OpaquePointer {
    std::uintptr_t address{};
};

template <typename T>
struct FieldResult {
    FieldStatus status{FieldStatus::unavailable};
    T value{};
    explicit operator bool() const noexcept { return status == FieldStatus::found; }
};

template <typename T>
struct ValueTypeOf;

template <> struct ValueTypeOf<bool> { static constexpr auto value = ValueType::boolean; };
template <> struct ValueTypeOf<std::int8_t> { static constexpr auto value = ValueType::i8; };
template <> struct ValueTypeOf<std::uint8_t> { static constexpr auto value = ValueType::u8; };
template <> struct ValueTypeOf<std::int16_t> { static constexpr auto value = ValueType::i16; };
template <> struct ValueTypeOf<std::uint16_t> { static constexpr auto value = ValueType::u16; };
template <> struct ValueTypeOf<std::int32_t> { static constexpr auto value = ValueType::i32; };
template <> struct ValueTypeOf<std::uint32_t> { static constexpr auto value = ValueType::u32; };
template <> struct ValueTypeOf<std::int64_t> { static constexpr auto value = ValueType::i64; };
template <> struct ValueTypeOf<std::uint64_t> { static constexpr auto value = ValueType::u64; };
template <> struct ValueTypeOf<float> { static constexpr auto value = ValueType::f32; };
template <> struct ValueTypeOf<double> { static constexpr auto value = ValueType::f64; };
template <> struct ValueTypeOf<OpaquePointer> { static constexpr auto value = ValueType::pointer; };

template <typename T>
concept ReadableValue = std::is_trivially_copyable_v<T> &&
    requires { ValueTypeOf<T>::value; };

}
