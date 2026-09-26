#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <chrono>
#include <cstdint>

namespace izanagi {

// o contexto e seus ponteiros valem apenas durante uma chamada de present.
struct FrameContext {
    IDXGISwapChain* swapchain{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    ID3D11RenderTargetView* render_target{};
    HWND window{};
    std::uint64_t frame_index{};
    std::chrono::steady_clock::duration delta_time{};
};

}
