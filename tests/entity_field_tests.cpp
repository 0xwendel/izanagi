#include "entities/entity_types.hpp"
#include "entities/field_reader.hpp"
#include "reflection/schema_registry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace izanagi;

static bool check(bool value, const char* label)
{
    if (!value) std::fprintf(stderr, "failed: %s\n", label);
    return value;
}

int main()
{
    bool ok = true;
    const auto packed = (42U << 15) | 17U;
    const auto handle = entities::DecodeHandle(packed, 17);
    ok &= check(handle && *handle == entities::EntityHandle{17, 42}, "valid handle");
    ok &= check(!entities::DecodeHandle(packed, 18), "wrong slot");
    ok &= check(!entities::DecodeHandle(packed, 32768), "slot out of range");
    ok &= check(!entities::DecodeHandle(0xffffffff, 32767), "invalid handle");
    ok &= check(handle && *handle != entities::EntityHandle{17, 43}, "stale serial");

    auto registry = schema::Registry::Build({
        {"client.dll", "Root", 64, {},
         {{"base_value", "int32", 8}, {"tiny", "uint8", 32},
          {"container", "CUtlVector", 40}}},
        {"client.dll", "Other", 64, {}, {{"base_value", "int32", 12}}},
        {"client.dll", "Derived", 128,
         {{"client.dll", "Root", 16}}, {{"own", "bool", 80}}},
        {"client.dll", "Ambiguous", 192,
         {{"client.dll", "Root", 0}, {"client.dll", "Other", 64}}, {}},
    }, 1);
    if (!check(registry != nullptr, "registry")) return 1;
    std::array<std::byte, 192> object{};
    const std::int32_t base = 1234;
    const bool own = true;
    std::memcpy(object.data() + 24, &base, sizeof(base));
    std::memcpy(object.data() + 80, &own, sizeof(own));
    auto read = [&](std::string_view cls, std::string_view field,
                    entities::ValueType type, void* target, std::size_t size) {
        return entities::detail::ReadField(*registry, "client.dll", cls, field,
            reinterpret_cast<std::uintptr_t>(object.data()), type, target, size);
    };
    std::int32_t number{};
    bool flag{};
    ok &= check(read("Derived", "base_value", entities::ValueType::i32,
                     &number, sizeof(number)) == entities::FieldStatus::found &&
                number == base, "inherited value");
    ok &= check(read("Derived", "own", entities::ValueType::boolean,
                     &flag, sizeof(flag)) == entities::FieldStatus::found && flag,
                "declared value");
    ok &= check(read("Derived", "absent", entities::ValueType::i32,
                     &number, sizeof(number)) == entities::FieldStatus::field_not_found,
                "missing field");
    ok &= check(read("Derived", "tiny", entities::ValueType::i32,
                     &number, sizeof(number)) == entities::FieldStatus::type_mismatch,
                "type mismatch");
    ok &= check(read("Derived", "base_value", entities::ValueType::i32,
                     &number, 1) == entities::FieldStatus::type_mismatch,
                "type size mismatch");
    ok &= check(read("Derived", "container", entities::ValueType::i32,
                     &number, sizeof(number)) == entities::FieldStatus::unsupported_type,
                "unsupported container");
    ok &= check(read("Ambiguous", "base_value", entities::ValueType::i32,
                     &number, sizeof(number)) == entities::FieldStatus::ambiguous_field,
                "ambiguous field");
    ok &= check(entities::detail::ReadField(*registry, "client.dll", "Derived",
                "base_value", (std::numeric_limits<std::uintptr_t>::max)() - 4,
                entities::ValueType::i32, &number, sizeof(number)) ==
                entities::FieldStatus::invalid_address, "address overflow");
    const std::array<float, 3> position{1.0f, 2.0f, 3.0f};
    std::memcpy(object.data() + 88, position.data(), sizeof(position));
    entities::FieldBinding binding{1, 88, 128, sizeof(position),
                                   entities::ValueType::bytes, "CGameSceneNode"};
    std::array<float, 3> copied{};
    ok &= check(entities::detail::ReadBoundMemory(
                    reinterpret_cast<std::uintptr_t>(object.data()), binding,
                    copied.data(), sizeof(copied)) == entities::FieldStatus::found &&
                copied == position, "bound vector read");
    binding.effective_offset = 124;
    ok &= check(entities::detail::ReadBoundMemory(
                    reinterpret_cast<std::uintptr_t>(object.data()), binding,
                    copied.data(), sizeof(copied)) ==
                entities::FieldStatus::invalid_address, "bound vector bounds");
    return ok ? 0 : 1;
}
