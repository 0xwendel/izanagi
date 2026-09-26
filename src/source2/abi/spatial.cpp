#include "source2/abi/spatial.hpp"

#include "entities/field_reader.hpp"
#include <string_view>

namespace izanagi::source2::abi {
namespace {

constexpr std::string_view k_scope = "client.dll";
constexpr std::string_view k_scene_node = "CGameSceneNode";
constexpr std::string_view k_node_field = "m_pGameSceneNode";
constexpr std::string_view k_position_field = "m_vecAbsOrigin";

spatial::Status Map(entities::FieldStatus status) noexcept
{
    switch (status) {
    case entities::FieldStatus::found: return spatial::Status::success;
    case entities::FieldStatus::entity_invalid: return spatial::Status::entity_invalid;
    case entities::FieldStatus::schema_unavailable: return spatial::Status::schema_unavailable;
    case entities::FieldStatus::invalid_address:
    case entities::FieldStatus::read_failed: return spatial::Status::invalid_pointer;
    case entities::FieldStatus::type_mismatch:
    case entities::FieldStatus::unsupported_type: return spatial::Status::type_mismatch;
    default: return spatial::Status::field_unavailable;
    }
}

spatial::Status Map(schema::Status status) noexcept
{
    switch (status) {
    case schema::Status::scope_unavailable:
    case schema::Status::registry_unavailable: return spatial::Status::schema_unavailable;
    case schema::Status::ambiguous: return spatial::Status::unsupported_layout;
    default: return spatial::Status::field_unavailable;
    }
}

}

void TransformResolver::Reset() noexcept
{
    entity_fields_.clear();
    node_field_ = {};
    node_bound_ = false;
    generation_ = 0;
}

TransformResolver::Binding TransformResolver::BindEntity(
    const schema::Registry& registry, std::string_view class_name)
{
    Binding result;
    const auto cls = registry.FindClass(k_scope, class_name);
    if (!cls) { result.status = Map(cls.status); return result; }
    const auto field = registry.FindField(k_scope, class_name, k_node_field);
    if (!field) { result.status = Map(field.status); return result; }
    const auto& type = field.value.field.type_name;
    if (type != "CGameSceneNode*") {
        result.status = spatial::Status::type_mismatch;
        return result;
    }
    result.field = {registry.generation(), field.value.effective_offset,
                    cls.value.size, sizeof(entities::OpaquePointer),
                    entities::ValueType::pointer, std::string(class_name)};
    result.status = spatial::Status::success;
    return result;
}

TransformResolver::Binding TransformResolver::BindNode(const schema::Registry& registry)
{
    Binding result;
    const auto cls = registry.FindClass(k_scope, k_scene_node);
    if (!cls) { result.status = Map(cls.status); return result; }
    const auto field = registry.FindField(k_scope, k_scene_node, k_position_field);
    if (!field) { result.status = Map(field.status); return result; }
    if (field.value.field.type_name != "VectorWS") {
        result.status = spatial::Status::type_mismatch;
        return result;
    }
    result.field = {registry.generation(), field.value.effective_offset,
                    cls.value.size, sizeof(spatial::Vec3), entities::ValueType::bytes,
                    std::string(k_scene_node)};
    result.status = spatial::Status::success;
    return result;
}

spatial::Result<spatial::WorldTransform> TransformResolver::Resolve(
    const entities::EntityView& entity, const schema::Registry& registry) noexcept
{
    spatial::Result<spatial::WorldTransform> result;
    if (!registry.generation() || entity.schema_generation() != registry.generation()) {
        result.status = spatial::Status::schema_unavailable;
        return result;
    }
    try {
        if (generation_ != registry.generation()) {
            Reset();
            generation_ = registry.generation();
        }
        if (!node_bound_) {
            node_field_ = BindNode(registry);
            node_bound_ = true;
        }
        if (node_field_.status != spatial::Status::success) {
            result.status = node_field_.status;
            return result;
        }
        auto it = entity_fields_.find(entity.class_name());
        if (it == entity_fields_.end()) {
            std::string key(entity.class_name());
            it = entity_fields_.emplace(key, BindEntity(registry, key)).first;
        }
        if (it->second.status != spatial::Status::success) {
            result.status = it->second.status;
            return result;
        }
        const auto node = entity.read_bound<entities::OpaquePointer>(it->second.field);
        if (!node) { result.status = Map(node.status); return result; }
        if (!node.value.address) {
            result.status = spatial::Status::transform_unavailable;
            return result;
        }
        spatial::Vec3 position;
        const auto read = entities::detail::ReadBoundMemory(
            node.value.address, node_field_.field, &position, sizeof(position));
        if (read != entities::FieldStatus::found) {
            result.status = Map(read);
            return result;
        }
        const auto after = entity.read_bound<entities::OpaquePointer>(it->second.field);
        if (!after || after.value.address != node.value.address) {
            result.status = spatial::Status::entity_invalid;
            return result;
        }
        if (!spatial::Finite(position)) {
            result.status = spatial::Status::invalid_value;
            return result;
        }
        result.status = spatial::Status::success;
        result.value.position = position;
        return result;
    } catch (...) {
        result.status = spatial::Status::transform_unavailable;
        return result;
    }
}

}
