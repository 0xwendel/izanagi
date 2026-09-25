#include "dx11_hook.hpp"

#include "diagnostics.hpp"
#include "gui.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace izanagi::dx11_hook {
namespace {

using present_fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using resize_buffers_fn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

constexpr std::size_t k_present_vtable_index = 8;
constexpr std::size_t k_resize_buffers_vtable_index = 13;

constinit SRWLOCK g_lifecycle_lock = SRWLOCK_INIT;
constinit SRWLOCK g_graphics_lock = SRWLOCK_INIT;

constinit std::atomic<present_fn> g_original_present{nullptr};
constinit std::atomic<resize_buffers_fn> g_original_resize_buffers{nullptr};
constinit std::atomic<bool> g_shutting_down{true};
constinit std::atomic<std::uint32_t> g_active_callbacks{0};
constinit std::atomic<bool> g_first_present_logged{false};

constinit void* g_present_target = nullptr;
constinit void* g_resize_buffers_target = nullptr;

constinit bool g_initialize_attempted = false;
constinit bool g_initialized = false;
constinit bool g_minhook_initialized = false;
constinit bool g_present_created = false;
constinit bool g_resize_buffers_created = false;
constinit bool g_present_enabled = false;
constinit bool g_resize_buffers_enabled = false;
constinit bool g_unsafe_to_unload = false;

// identidade sem addref; reutilização do endereço pode confundir a seleção.
constinit IDXGISwapChain* g_target_swapchain = nullptr;
constinit ID3D11Device* g_device = nullptr;
constinit ID3D11DeviceContext* g_context = nullptr;
constinit ID3D11RenderTargetView* g_rtv = nullptr;
constinit std::uint32_t g_resize_callbacks = 0;

class exclusive_lock final {
public:
    explicit exclusive_lock(SRWLOCK& lock) noexcept : lock_(&lock)
    {
        AcquireSRWLockExclusive(lock_);
    }

    ~exclusive_lock() noexcept
    {
        ReleaseSRWLockExclusive(lock_);
    }

    exclusive_lock(const exclusive_lock&) = delete;
    exclusive_lock& operator=(const exclusive_lock&) = delete;

private:
    SRWLOCK* lock_;
};

class callback_guard final {
public:
    callback_guard() noexcept
    {
        g_active_callbacks.fetch_add(1, std::memory_order_acq_rel);
    }

    ~callback_guard() noexcept
    {
        g_active_callbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    callback_guard(const callback_guard&) = delete;
    callback_guard& operator=(const callback_guard&) = delete;
};

template <typename T>
void release_com(T*& value) noexcept
{
    T* const owned = std::exchange(value, nullptr);
    if (owned != nullptr) {
        owned->Release();
    }
}

void report_minhook_error(const char* operation, MH_STATUS status) noexcept
{
    report_error(operation, static_cast<unsigned long>(status));
}

class dummy_swapchain final {
public:
    dummy_swapchain() noexcept = default;

    ~dummy_swapchain() noexcept
    {
        (void)Cleanup();
    }

    dummy_swapchain(const dummy_swapchain&) = delete;
    dummy_swapchain& operator=(const dummy_swapchain&) = delete;

    bool Create() noexcept
    {
        instance_ = GetModuleHandleW(nullptr);
        if (instance_ == nullptr) {
            report_error("GetModuleHandleW(dummy)", GetLastError());
            return false;
        }

        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        // mantém a wndproc temporária fora da dll caso o cleanup falhe.
        window_class.lpfnWndProc = &DefWindowProcW;
        window_class.hInstance = instance_;
        window_class.lpszClassName = class_name_;

        atom_ = RegisterClassExW(&window_class);
        if (atom_ == 0) {
            report_error("RegisterClassExW(dummy)", GetLastError());
            return false;
        }

        window_ = CreateWindowExW(0, class_name_, L"", WS_OVERLAPPED,
                                  0, 0, 1, 1, nullptr, nullptr, instance_, nullptr);
        if (window_ == nullptr) {
            report_error("CreateWindowExW(dummy)", GetLastError());
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = 1;
        desc.BufferDesc.Height = 1;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = window_;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const HRESULT result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &desc, &swapchain_, &device_, nullptr, &context_);
        if (FAILED(result)) {
            report_error("D3D11CreateDeviceAndSwapChain(dummy)",
                         static_cast<unsigned long>(result));
            return false;
        }

        return true;
    }

    bool Resolve(void*& present, void*& resize_buffers) const noexcept
    {
        if (swapchain_ == nullptr) {
            return false;
        }

        void** const vtable = *reinterpret_cast<void***>(swapchain_);
        if (vtable == nullptr) {
            report_error("IDXGISwapChain vtable(dummy)", ERROR_INVALID_DATA);
            return false;
        }

        present = vtable[k_present_vtable_index];
        resize_buffers = vtable[k_resize_buffers_vtable_index];
        if (present == nullptr || resize_buffers == nullptr) {
            report_error("IDXGISwapChain methods(dummy)", ERROR_INVALID_ADDRESS);
            return false;
        }
        return true;
    }

    bool Cleanup() noexcept
    {
        bool clean = true;

        release_com(context_);
        release_com(swapchain_);
        release_com(device_);

        if (window_ != nullptr) {
            if (!DestroyWindow(window_)) {
                report_error("DestroyWindow(dummy)", GetLastError());
                clean = false;
            } else {
                window_ = nullptr;
            }
        }

        if (atom_ != 0) {
            if (!UnregisterClassW(class_name_, instance_)) {
                report_error("UnregisterClassW(dummy)", GetLastError());
                clean = false;
            } else {
                atom_ = 0;
            }
        }

        return clean;
    }

private:
    static constexpr wchar_t class_name_[] = L"izanagi.dx11.bootstrap";

    HINSTANCE instance_ = nullptr;
    ATOM atom_ = 0;
    HWND window_ = nullptr;
    IDXGISwapChain* swapchain_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
};

bool resolve_methods(void*& present, void*& resize_buffers) noexcept
{
    dummy_swapchain dummy;
    const bool created = dummy.Create();
    const bool resolved = created && dummy.Resolve(present, resize_buffers);
    const bool cleaned = dummy.Cleanup();
    return resolved && cleaned;
}

struct candidate_resources final {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    DXGI_SWAP_CHAIN_DESC desc{};

    ~candidate_resources() noexcept
    {
        release_com(context);
        release_com(device);
    }

    candidate_resources() noexcept = default;
    candidate_resources(const candidate_resources&) = delete;
    candidate_resources& operator=(const candidate_resources&) = delete;

    bool Inspect(IDXGISwapChain* swapchain) noexcept
    {
        if (swapchain == nullptr ||
            FAILED(swapchain->GetDevice(__uuidof(ID3D11Device),
                                        reinterpret_cast<void**>(&device))) ||
            device == nullptr) {
            return false;
        }

        if (FAILED(swapchain->GetDesc(&desc)) || desc.OutputWindow == nullptr ||
            !IsWindow(desc.OutputWindow)) {
            return false;
        }

        DWORD process_id = 0;
        if (GetWindowThreadProcessId(desc.OutputWindow, &process_id) == 0 ||
            process_id != GetCurrentProcessId()) {
            return false;
        }

        device->GetImmediateContext(&context);
        return context != nullptr;
    }
};

bool select_or_match_target(IDXGISwapChain* swapchain,
                            candidate_resources& candidate) noexcept
{
    bool needs_candidate = false;
    {
        exclusive_lock lock(g_graphics_lock);
        if (g_target_swapchain == swapchain) {
            return true;
        }
        if (g_target_swapchain != nullptr) {
            return false;
        }
        needs_candidate = true;
    }

    if (needs_candidate && !candidate.Inspect(swapchain)) {
        return false;
    }

    exclusive_lock lock(g_graphics_lock);
    if (g_shutting_down.load(std::memory_order_acquire)) {
        return false;
    }

    if (g_target_swapchain == nullptr) {
        g_target_swapchain = swapchain;
        g_device = std::exchange(candidate.device, nullptr);
        g_context = std::exchange(candidate.context, nullptr);
    }
    return g_target_swapchain == swapchain;
}

bool create_rtv_locked(IDXGISwapChain* swapchain) noexcept
{
    if (g_rtv != nullptr) {
        return true;
    }
    if (g_device == nullptr || g_resize_callbacks != 0) {
        return false;
    }

    ID3D11Texture2D* backbuffer = nullptr;
    const HRESULT get_result = swapchain->GetBuffer(
        0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer));
    if (FAILED(get_result) || backbuffer == nullptr) {
        if (FAILED(get_result)) {
            report_error("IDXGISwapChain::GetBuffer",
                         static_cast<unsigned long>(get_result));
        }
        release_com(backbuffer);
        return false;
    }

    const HRESULT create_result =
        g_device->CreateRenderTargetView(backbuffer, nullptr, &g_rtv);
    release_com(backbuffer);

    if (FAILED(create_result) || g_rtv == nullptr) {
        if (FAILED(create_result)) {
            report_error("ID3D11Device::CreateRenderTargetView",
                         static_cast<unsigned long>(create_result));
        }
        release_com(g_rtv);
        return false;
    }
    return true;
}

void log_first_present() noexcept
{
    if (!g_first_present_logged.exchange(true, std::memory_order_acq_rel)) {
        (void)std::fputs("septhen: first present hook hit\n", stdout);
        (void)std::fflush(stdout);
    }
}

void release_bound_rtv_locked() noexcept
{
    if (g_rtv == nullptr || g_context == nullptr) {
        release_com(g_rtv);
        return;
    }

    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
        bound_rtvs{};
    ID3D11DepthStencilView* bound_dsv = nullptr;
    g_context->OMGetRenderTargets(static_cast<UINT>(bound_rtvs.size()),
                                  bound_rtvs.data(), &bound_dsv);

    bool found = false;
    UINT last_non_null = 0;
    for (UINT index = 0; index < static_cast<UINT>(bound_rtvs.size()); ++index) {
        if (bound_rtvs[index] == g_rtv) {
            bound_rtvs[index]->Release();
            bound_rtvs[index] = nullptr;
            found = true;
        }
        if (bound_rtvs[index] != nullptr) {
            last_non_null = index + 1;
        }
    }

    if (found) {
        ID3D11RenderTargetView* const* const restored_rtvs =
            last_non_null == 0 ? nullptr : bound_rtvs.data();
        g_context->OMSetRenderTargets(last_non_null, restored_rtvs, bound_dsv);
    }

    for (ID3D11RenderTargetView*& rtv : bound_rtvs) {
        release_com(rtv);
    }
    release_com(bound_dsv);
    release_com(g_rtv);
}

class resize_guard final {
public:
    explicit resize_guard(bool active) noexcept : active_(active) {}

    ~resize_guard() noexcept
    {
        if (!active_) {
            return;
        }

        exclusive_lock lock(g_graphics_lock);
        if (g_resize_callbacks != 0) {
            --g_resize_callbacks;
        }
    }

    resize_guard(const resize_guard&) = delete;
    resize_guard& operator=(const resize_guard&) = delete;

private:
    bool active_;
};

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* swapchain,
                                         UINT sync_interval, UINT flags) noexcept
{
    callback_guard callback;
    const present_fn original = g_original_present.load(std::memory_order_acquire);
    if (original == nullptr) {
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!g_shutting_down.load(std::memory_order_acquire) &&
        !gui::InGuiCall()) {
        try {
            candidate_resources candidate;
            if (select_or_match_target(swapchain, candidate)) {
                exclusive_lock lock(g_graphics_lock);
                if (!g_shutting_down.load(std::memory_order_acquire) &&
                    g_target_swapchain == swapchain &&
                    create_rtv_locked(swapchain)) {
                    log_first_present();

                    if (!gui::IsInitialized()) {
                        DXGI_SWAP_CHAIN_DESC desc{};
                        DWORD process_id = 0;
                        if (SUCCEEDED(swapchain->GetDesc(&desc)) &&
                            desc.OutputWindow != nullptr &&
                            IsWindow(desc.OutputWindow) &&
                            GetWindowThreadProcessId(desc.OutputWindow,
                                                     &process_id) != 0 &&
                            process_id == GetCurrentProcessId()) {
                            (void)gui::Initialize(desc.OutputWindow, g_device, g_context);
                        }
                    }

                    if (gui::IsInitialized()) {
                        gui::Render(g_rtv);
                    }
                }
            }
        } catch (...) {
            report_error("hooked_present", ERROR_UNHANDLED_EXCEPTION);
        }
    }

    try {
        return original(swapchain, sync_interval, flags);
    } catch (...) {
        report_error("IDXGISwapChain::Present(original)", ERROR_UNHANDLED_EXCEPTION);
        return E_FAIL;
    }
}

HRESULT STDMETHODCALLTYPE hooked_resize_buffers(IDXGISwapChain* swapchain,
                                                UINT buffer_count, UINT width,
                                                UINT height, DXGI_FORMAT format,
                                                UINT swapchain_flags) noexcept
{
    callback_guard callback;
    const resize_buffers_fn original =
        g_original_resize_buffers.load(std::memory_order_acquire);
    if (original == nullptr) {
        return DXGI_ERROR_INVALID_CALL;
    }

    bool tracks_target_resize = false;
    if (!g_shutting_down.load(std::memory_order_acquire) &&
        !gui::InGuiCall()) {
        try {
            candidate_resources candidate;
            if (select_or_match_target(swapchain, candidate)) {
                exclusive_lock lock(g_graphics_lock);
                if (!g_shutting_down.load(std::memory_order_acquire) &&
                    g_target_swapchain == swapchain) {
                    ++g_resize_callbacks;
                    tracks_target_resize = true;
                    release_bound_rtv_locked();
                }
            }
        } catch (...) {
            report_error("hooked_resize_buffers", ERROR_UNHANDLED_EXCEPTION);
        }
    }

    resize_guard resize(tracks_target_resize);
    try {
        return original(swapchain, buffer_count, width, height, format,
                        swapchain_flags);
    } catch (...) {
        report_error("IDXGISwapChain::ResizeBuffers(original)",
                     ERROR_UNHANDLED_EXCEPTION);
        return E_FAIL;
    }
}

bool disable_hook(void* target, bool created, bool& enabled,
                  const char* operation) noexcept
{
    if (!created || !enabled) {
        return true;
    }

    const MH_STATUS status = MH_DisableHook(target);
    if (status == MH_OK || status == MH_ERROR_DISABLED) {
        enabled = false;
        return true;
    }

    report_minhook_error(operation, status);
    return false;
}

bool remove_hook(void* target, bool& created, const char* operation) noexcept
{
    if (!created) {
        return true;
    }

    const MH_STATUS status = MH_RemoveHook(target);
    if (status == MH_OK || status == MH_ERROR_NOT_CREATED) {
        created = false;
        return true;
    }

    report_minhook_error(operation, status);
    return false;
}

void drain_callbacks_with_grace() noexcept
{
    do {
        while (g_active_callbacks.load(std::memory_order_acquire) != 0) {
            Sleep(1);
        }
        Sleep(20);
    } while (g_active_callbacks.load(std::memory_order_acquire) != 0);
}

bool rollback_hooks() noexcept
{
    bool disabled = true;
    disabled = disable_hook(g_resize_buffers_target, g_resize_buffers_created,
                            g_resize_buffers_enabled,
                            "MH_DisableHook(ResizeBuffers rollback)") && disabled;
    disabled = disable_hook(g_present_target, g_present_created, g_present_enabled,
                            "MH_DisableHook(Present rollback)") && disabled;

    if (!disabled) {
        g_unsafe_to_unload = true;
        return false;
    }
    drain_callbacks_with_grace();

    bool clean = true;
    clean = remove_hook(g_resize_buffers_target, g_resize_buffers_created,
                        "MH_RemoveHook(ResizeBuffers rollback)") && clean;
    clean = remove_hook(g_present_target, g_present_created,
                        "MH_RemoveHook(Present rollback)") && clean;

    if (g_minhook_initialized) {
        const MH_STATUS status = MH_Uninitialize();
        if (status == MH_OK) {
            g_minhook_initialized = false;
        } else {
            report_minhook_error("MH_Uninitialize(rollback)", status);
            clean = false;
        }
    }

    if (!clean) {
        g_unsafe_to_unload = true;
    }
    return clean;
}

void release_graphics_state() noexcept
{
    exclusive_lock lock(g_graphics_lock);
    release_bound_rtv_locked();
    release_com(g_context);
    release_com(g_device);
    g_target_swapchain = nullptr;
    g_resize_callbacks = 0;
}

void report_unsafe_shutdown(const char* reason) noexcept
{
    (void)std::fputs("izanagi: unsafe hook shutdown; module remains loaded: ",
                     stderr);
    (void)std::fputs(reason, stderr);
    (void)std::fputc('\n', stderr);
    (void)std::fflush(stderr);
}

}

bool Initialize() noexcept
{
    exclusive_lock lifecycle(g_lifecycle_lock);
    if (g_initialized) {
        return true;
    }
    if (g_initialize_attempted) {
        return false;
    }

    g_initialize_attempted = true;
    g_shutting_down.store(true, std::memory_order_release);

    if (!resolve_methods(g_present_target, g_resize_buffers_target)) {
        return false;
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        report_minhook_error("MH_Initialize", status);
        return false;
    }
    g_minhook_initialized = true;

    void* original = nullptr;
    status = MH_CreateHook(g_present_target,
                           reinterpret_cast<void*>(&hooked_present), &original);
    if (status != MH_OK) {
        report_minhook_error("MH_CreateHook(Present)", status);
        (void)rollback_hooks();
        return false;
    }
    g_present_created = true;
    g_original_present.store(reinterpret_cast<present_fn>(original),
                             std::memory_order_release);

    original = nullptr;
    status = MH_CreateHook(g_resize_buffers_target,
                           reinterpret_cast<void*>(&hooked_resize_buffers),
                           &original);
    if (status != MH_OK) {
        report_minhook_error("MH_CreateHook(ResizeBuffers)", status);
        (void)rollback_hooks();
        return false;
    }
    g_resize_buffers_created = true;
    g_original_resize_buffers.store(reinterpret_cast<resize_buffers_fn>(original),
                                    std::memory_order_release);

    status = MH_EnableHook(g_present_target);
    if (status != MH_OK) {
        g_present_enabled = status == MH_ERROR_ENABLED;
        report_minhook_error("MH_EnableHook(Present)", status);
        (void)rollback_hooks();
        return false;
    }
    g_present_enabled = true;

    status = MH_EnableHook(g_resize_buffers_target);
    if (status != MH_OK) {
        g_resize_buffers_enabled = status == MH_ERROR_ENABLED;
        report_minhook_error("MH_EnableHook(ResizeBuffers)", status);
        (void)rollback_hooks();
        return false;
    }
    g_resize_buffers_enabled = true;

    g_initialized = true;
    g_shutting_down.store(false, std::memory_order_release);
    return true;
}

bool Shutdown() noexcept
{
    exclusive_lock lifecycle(g_lifecycle_lock);
    g_shutting_down.store(true, std::memory_order_release);

    // evita restaurar a wndproc enquanto present ainda pode instalá-la.
    {
        exclusive_lock graphics(g_graphics_lock);
        release_bound_rtv_locked();
    }

    if (!gui::DetachWndProcAndDrain()) {
        report_error("gui::DetachWndProcAndDrain", ERROR_BUSY);
        report_unsafe_shutdown("WndProc restoration was not proven");
        g_unsafe_to_unload = true;

        bool disabled = true;
        disabled = disable_hook(g_present_target, g_present_created,
                                g_present_enabled,
                                "MH_DisableHook(Present after WndProc failure)") &&
                   disabled;
        disabled = disable_hook(
                       g_resize_buffers_target, g_resize_buffers_created,
                       g_resize_buffers_enabled,
                       "MH_DisableHook(ResizeBuffers after WndProc failure)") &&
                   disabled;
        if (disabled) {
            while (g_active_callbacks.load(std::memory_order_acquire) != 0) {
                Sleep(1);
            }
        }
        return false;
    }

    bool disabled = true;
    disabled = disable_hook(g_present_target, g_present_created,
                            g_present_enabled,
                            "MH_DisableHook(Present)") && disabled;
    disabled = disable_hook(g_resize_buffers_target, g_resize_buffers_created,
                            g_resize_buffers_enabled,
                            "MH_DisableHook(ResizeBuffers)") && disabled;
    if (!disabled) {
        report_unsafe_shutdown("a DXGI hook could not be disabled");
        g_unsafe_to_unload = true;
        return false;
    }

    drain_callbacks_with_grace();

    gui::Shutdown();
    release_graphics_state();

    bool removed = true;
    removed = remove_hook(g_present_target, g_present_created,
                          "MH_RemoveHook(Present)") && removed;
    removed = remove_hook(g_resize_buffers_target, g_resize_buffers_created,
                          "MH_RemoveHook(ResizeBuffers)") && removed;

    bool uninitialized = true;
    if (g_minhook_initialized) {
        const MH_STATUS status = MH_Uninitialize();
        if (status == MH_OK) {
            g_minhook_initialized = false;
        } else {
            report_minhook_error("MH_Uninitialize", status);
            uninitialized = false;
        }
    }

    const bool safe = removed && uninitialized && !g_unsafe_to_unload;
    if (safe) {
        g_initialized = false;
    } else {
        report_unsafe_shutdown("MinHook teardown was not proven complete");
        g_unsafe_to_unload = true;
    }
    return safe;
}

}
