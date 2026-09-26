#pragma once

#include <Windows.h>
#include <d3d11.h>
#include "frame_context.hpp"

namespace izanagi::gui {

bool Initialize(HWND hwnd, ID3D11Device* device,
                ID3D11DeviceContext* context) noexcept;
void Render(const FrameContext& frame) noexcept;

bool DetachWndProcAndDrain() noexcept;
void Shutdown() noexcept;
bool IsInitialized() noexcept;
bool InGuiCall() noexcept;

}
