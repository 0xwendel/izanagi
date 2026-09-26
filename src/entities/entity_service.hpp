#pragma once

#include "entities/entity_types.hpp"
#include "entities/entity_view.hpp"
#include "frame_context.hpp"
#include <cstdint>
#include <memory>
#include <optional>

namespace izanagi::entities {

bool Initialize() noexcept;
void Poll() noexcept;
void OnModulesUpdated() noexcept;
void Tick(const FrameContext& frame) noexcept;
void BeginShutdown() noexcept;
void Shutdown() noexcept;
State CurrentState() noexcept;
Failure LastFailure() noexcept;
double LastTickMs() noexcept;
const char* StateName(State state) noexcept;
const char* FailureName(Failure failure) noexcept;
std::shared_ptr<const EntityFrameSnapshot> Snapshot() noexcept;
std::optional<EntityView> Resolve(EntityHandle handle) noexcept;

}
