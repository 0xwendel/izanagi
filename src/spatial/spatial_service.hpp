#pragma once

#include "spatial/spatial_types.hpp"
#include <memory>

namespace izanagi::spatial {

bool Initialize() noexcept;
void Poll() noexcept;
void BeginShutdown() noexcept;
void Shutdown() noexcept;
ServiceState CurrentState() noexcept;
std::shared_ptr<const SpatialFrameSnapshot> Snapshot() noexcept;

}
