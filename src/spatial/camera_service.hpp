#pragma once

#include "frame_context.hpp"
#include "spatial/spatial_types.hpp"

namespace izanagi::spatial::camera {

bool Initialize() noexcept;
void Tick(const FrameContext& frame) noexcept;
CameraSnapshot Snapshot() noexcept;
ServiceState CurrentState() noexcept;
void BeginShutdown() noexcept;
void Shutdown() noexcept;

}
