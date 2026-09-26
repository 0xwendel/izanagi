#pragma once

#include "spatial/spatial_types.hpp"

namespace izanagi::spatial {

bool ValidCamera(const CameraSnapshot& camera) noexcept;
ProjectionResult Project(Vec3 world, const CameraSnapshot& camera) noexcept;

}
