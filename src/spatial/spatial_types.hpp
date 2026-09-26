#pragma once

#include "entities/entity_types.hpp"
#include "spatial/math.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace izanagi::spatial {

enum class Status : std::uint8_t {
    success, entity_invalid, schema_unavailable, transform_unavailable,
    field_unavailable, type_mismatch, invalid_pointer, invalid_value,
    unsupported_layout
};

template <typename T>
struct Result {
    Status status{Status::transform_unavailable};
    T value{};
    explicit operator bool() const noexcept { return status == Status::success; }
};

struct WorldTransform { Vec3 position; };

enum class ServiceState : std::uint8_t {
    uninitialized, resolving, ready, unavailable, shutting_down, stopped
};

struct CameraSnapshot {
    Matrix4x4 view_projection;
    std::uint32_t viewport_width{};
    std::uint32_t viewport_height{};
    std::uint64_t frame_index{};
    std::uint64_t generation{};
    bool valid{};
};

enum class ProjectionStatus : std::uint8_t {
    visible, outside_viewport, behind_camera, invalid_camera, invalid_point
};

struct ProjectionResult {
    ProjectionStatus status{ProjectionStatus::invalid_camera};
    Vec2 screen{};
    float depth{};
};

struct SpatialEntitySnapshot {
    entities::EntityHandle handle;
    std::string class_name;
    Status transform_status{Status::transform_unavailable};
    WorldTransform transform;
    ProjectionResult projection;
};

struct SpatialFrameSnapshot {
    std::uint64_t frame_index{};
    std::uint64_t entity_generation{};
    std::uint64_t schema_generation{};
    CameraSnapshot camera;
    std::vector<SpatialEntitySnapshot> entities;
    std::size_t valid_transforms{};
    std::size_t projected{};
    std::size_t offscreen{};
    std::size_t behind_camera{};
    double tick_ms{};
};

const char* StatusName(Status status) noexcept;
const char* ServiceStateName(ServiceState state) noexcept;
const char* ProjectionStatusName(ProjectionStatus status) noexcept;

}
