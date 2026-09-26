#pragma once

#include "entities/entity_view.hpp"
#include "reflection/schema_registry.hpp"
#include "spatial/spatial_types.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace izanagi::source2::abi {

class TransformResolver final {
public:
    spatial::Result<spatial::WorldTransform> Resolve(
        const entities::EntityView& entity,
        const schema::Registry& registry) noexcept;
    void Reset() noexcept;

private:
    struct Hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view value) const noexcept
        { return std::hash<std::string_view>{}(value); }
    };
    struct Binding {
        spatial::Status status{spatial::Status::field_unavailable};
        entities::FieldBinding field;
    };
    Binding BindEntity(const schema::Registry& registry,
                       std::string_view class_name);
    Binding BindNode(const schema::Registry& registry);

    std::uint64_t generation_{};
    std::unordered_map<std::string, Binding, Hash, std::equal_to<>> entity_fields_;
    Binding node_field_;
    bool node_bound_{};
};

}
