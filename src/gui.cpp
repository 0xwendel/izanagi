#include "gui.hpp"

#include "diagnostics.hpp"
#include "runtime.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd,
                                                             UINT message,
                                                             WPARAM w_param,
                                                             LPARAM l_param);

namespace izanagi::gui {
namespace {

enum class phase : std::uint8_t {
  uninitialized,
  initializing,
  ready,
  shutting_down,
  failed,
};

// o backend win32 pode reentrar a wndproc durante uma chamada síncrona.
constinit CRITICAL_SECTION g_imgui_lock{};
constinit std::atomic<bool> g_lock_initialized{false};
constinit std::atomic<phase> g_phase{phase::uninitialized};
constinit std::atomic<bool> g_shutting_down{false};
constinit std::atomic<std::uint32_t> g_active_wndproc_callbacks{0};
constinit std::atomic<WNDPROC> g_previous_wndproc{nullptr};
constinit std::atomic<bool> g_wndproc_installed{false};
constinit std::atomic<bool> g_wndproc_was_installed{false};
constinit std::atomic<bool> g_wndproc_detached_safely{false};

constinit HWND g_hwnd = nullptr;
constinit ID3D11DeviceContext *g_context = nullptr;
constinit bool g_context_created = false;
constinit bool g_win32_initialized = false;
constinit bool g_dx11_initialized = false;
constinit bool g_menu_open = false;
constinit thread_local std::uint32_t g_imgui_lock_depth = 0;

LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT message, WPARAM w_param,
                               LPARAM l_param) noexcept;

class critical_section_guard final {
public:
  explicit critical_section_guard(CRITICAL_SECTION &lock) noexcept
      : lock_(&lock) {
    EnterCriticalSection(lock_);
    ++g_imgui_lock_depth;
  }

  ~critical_section_guard() noexcept {
    LeaveCriticalSection(lock_);
    --g_imgui_lock_depth;
  }

  critical_section_guard(const critical_section_guard &) = delete;
  critical_section_guard &operator=(const critical_section_guard &) = delete;

private:
  CRITICAL_SECTION *lock_;
};

class wndproc_callback_guard final {
public:
  wndproc_callback_guard() noexcept {
    g_active_wndproc_callbacks.fetch_add(1, std::memory_order_acq_rel);
  }

  ~wndproc_callback_guard() noexcept {
    g_active_wndproc_callbacks.fetch_sub(1, std::memory_order_acq_rel);
  }

  wndproc_callback_guard(const wndproc_callback_guard &) = delete;
  wndproc_callback_guard &operator=(const wndproc_callback_guard &) = delete;
};

class om_bindings_guard final {
public:
  explicit om_bindings_guard(ID3D11DeviceContext *context) noexcept
      : context_(context) {
    context_->OMGetRenderTargets(static_cast<UINT>(render_targets_.size()),
                                 render_targets_.data(), &depth_stencil_);
  }

  ~om_bindings_guard() noexcept {
    context_->OMSetRenderTargets(static_cast<UINT>(render_targets_.size()),
                                 render_targets_.data(), depth_stencil_);

    for (ID3D11RenderTargetView *&view : render_targets_) {
      if (view != nullptr) {
        view->Release();
        view = nullptr;
      }
    }
    if (depth_stencil_ != nullptr) {
      depth_stencil_->Release();
      depth_stencil_ = nullptr;
    }
  }

  om_bindings_guard(const om_bindings_guard &) = delete;
  om_bindings_guard &operator=(const om_bindings_guard &) = delete;

private:
  ID3D11DeviceContext *context_;
  std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
      render_targets_{};
  ID3D11DepthStencilView *depth_stencil_ = nullptr;
};

void report_gui_error(const char *operation, DWORD error) noexcept {
  report_error(operation, error);
}

void report_unsafe_wndproc(const char *operation, DWORD error) noexcept {
  report_error(operation, error);
  try {
    std::cerr << "izanagi: cannot prove WndProc removal; module remains loaded"
              << " (" << operation << ", code=" << error << ")\n"
              << std::flush;
  } catch (...) {
  }
}

bool read_wndproc(HWND hwnd, WNDPROC &procedure, DWORD &error) noexcept {
  SetLastError(ERROR_SUCCESS);
  const LONG_PTR value = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
  error = GetLastError();
  if (value == 0) {
    if (error == ERROR_SUCCESS) {
      error = ERROR_INVALID_FUNCTION;
    }
    procedure = nullptr;
    return false;
  }

  procedure = reinterpret_cast<WNDPROC>(value);
  error = ERROR_SUCCESS;
  return true;
}

bool is_mouse_input(UINT message) noexcept {
  switch (message) {
  case WM_MOUSEMOVE:
  case WM_LBUTTONDOWN:
  case WM_LBUTTONUP:
  case WM_LBUTTONDBLCLK:
  case WM_RBUTTONDOWN:
  case WM_RBUTTONUP:
  case WM_RBUTTONDBLCLK:
  case WM_MBUTTONDOWN:
  case WM_MBUTTONUP:
  case WM_MBUTTONDBLCLK:
  case WM_XBUTTONDOWN:
  case WM_XBUTTONUP:
  case WM_XBUTTONDBLCLK:
  case WM_MOUSEWHEEL:
  case WM_MOUSEHWHEEL:
    return true;
  default:
    return false;
  }
}

bool is_keyboard_input(UINT message) noexcept {
  switch (message) {
  case WM_KEYDOWN:
  case WM_KEYUP:
  case WM_SYSKEYDOWN:
  case WM_SYSKEYUP:
  case WM_CHAR:
  case WM_SYSCHAR:
  case WM_DEADCHAR:
  case WM_SYSDEADCHAR:
  case WM_UNICHAR:
    return true;
  default:
    return false;
  }
}

LRESULT forward_to_host(HWND hwnd, UINT message, WPARAM w_param,
                        LPARAM l_param) noexcept {
  const WNDPROC previous = g_previous_wndproc.load(std::memory_order_acquire);
  if (previous == nullptr) {
    report_gui_error("HookedWndProc: previous WndProc is null",
                     ERROR_INVALID_FUNCTION);
    return DefWindowProcW(hwnd, message, w_param, l_param);
  }
  try {
    return CallWindowProcW(previous, hwnd, message, w_param, l_param);
  } catch (...) {
    report_gui_error("CallWindowProcW exception", ERROR_UNHANDLED_EXCEPTION);
    return DefWindowProcW(hwnd, message, w_param, l_param);
  }
}

void destroy_imgui_locked() noexcept {
  if (g_dx11_initialized) {
    ImGui_ImplDX11_Shutdown();
    g_dx11_initialized = false;
  }
  if (g_win32_initialized) {
    ImGui_ImplWin32_Shutdown();
    g_win32_initialized = false;
  }
  if (g_context_created) {
    ImGui::DestroyContext();
    g_context_created = false;
  }

  g_context = nullptr;
  g_hwnd = nullptr;
  g_menu_open = false;
}

bool restore_wndproc_locked() noexcept {
  if (!g_wndproc_installed.load(std::memory_order_acquire)) {
    return true;
  }

  const HWND hwnd = g_hwnd;
  const WNDPROC previous = g_previous_wndproc.load(std::memory_order_acquire);
  if (hwnd == nullptr || previous == nullptr || !IsWindow(hwnd)) {
    report_unsafe_wndproc("WndProc restore: invalid window or predecessor",
                          ERROR_INVALID_WINDOW_HANDLE);
    return false;
  }

  WNDPROC current = nullptr;
  DWORD error = ERROR_SUCCESS;
  if (!read_wndproc(hwnd, current, error)) {
    report_unsafe_wndproc("GetWindowLongPtrW(GWLP_WNDPROC)", error);
    return false;
  }
  if (current != &HookedWndProc) {
    report_unsafe_wndproc("WndProc chain changed externally", ERROR_BUSY);
    return false;
  }

  SetLastError(ERROR_SUCCESS);
  const LONG_PTR displaced = SetWindowLongPtrW(
      hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
  error = GetLastError();
  if (displaced == 0 && error != ERROR_SUCCESS) {
    report_unsafe_wndproc("SetWindowLongPtrW(restore)", error);
    return false;
  }
  if (reinterpret_cast<WNDPROC>(displaced) != &HookedWndProc) {
    report_unsafe_wndproc("SetWindowLongPtrW(restore): concurrent change",
                          ERROR_BUSY);
    return false;
  }

  WNDPROC verified = nullptr;
  if (!read_wndproc(hwnd, verified, error)) {
    report_unsafe_wndproc("GetWindowLongPtrW(verify restore)", error);
    return false;
  }
  if (verified != previous) {
    report_unsafe_wndproc("WndProc restore verification", ERROR_BUSY);
    return false;
  }

  g_wndproc_installed.store(false, std::memory_order_release);
  return true;
}

void wait_for_wndproc_drain() noexcept {
  while (g_active_wndproc_callbacks.load(std::memory_order_acquire) != 0) {
    Sleep(1);
  }
}

bool synchronize_window_thread(HWND hwnd) noexcept {
  if (hwnd == nullptr || !IsWindow(hwnd)) {
    report_unsafe_wndproc("WndProc drain barrier: invalid window",
                          ERROR_INVALID_WINDOW_HANDLE);
    return false;
  }

  DWORD_PTR result = 0;
  SetLastError(ERROR_SUCCESS);
  if (SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                          2000, &result) == 0) {
    DWORD error = GetLastError();
    if (error == ERROR_SUCCESS) {
      error = ERROR_TIMEOUT;
    }
    report_unsafe_wndproc("SendMessageTimeoutW(WM_NULL drain barrier)", error);
    return false;
  }
  return true;
}

LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT message, WPARAM w_param,
                               LPARAM l_param) noexcept {
  wndproc_callback_guard callback_guard;

  if (g_shutting_down.load(std::memory_order_acquire) || InGuiCall()) {
    return forward_to_host(hwnd, message, w_param, l_param);
  }

  bool consume = false;
  try {
    if (!g_lock_initialized.load(std::memory_order_acquire)) {
      return forward_to_host(hwnd, message, w_param, l_param);
    }

    {
      critical_section_guard lock(g_imgui_lock);
      if (g_shutting_down.load(std::memory_order_acquire) ||
          g_phase.load(std::memory_order_acquire) != phase::ready ||
          hwnd != g_hwnd || !g_context_created || !g_win32_initialized) {
      } else {
        (void)ImGui_ImplWin32_WndProcHandler(hwnd, message, w_param, l_param);

        const bool insert_down =
            w_param == VK_INSERT &&
            (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
        const bool insert_up = w_param == VK_INSERT &&
                               (message == WM_KEYUP || message == WM_SYSKEYUP);
        const bool insert_transition =
            insert_down &&
            (static_cast<std::uint64_t>(l_param) & (1ULL << 30)) == 0;

        if (insert_transition) {
          g_menu_open = !g_menu_open;
        }

        if (insert_down || insert_up) {
          consume = true;
        } else if (g_menu_open) {
          const ImGuiIO &io = ImGui::GetIO();
          consume = (is_mouse_input(message) && io.WantCaptureMouse) ||
                    (is_keyboard_input(message) && io.WantCaptureKeyboard);
        }
      }
    }
  } catch (...) {
    report_gui_error("HookedWndProc exception", ERROR_UNHANDLED_EXCEPTION);
    consume = false;
  }

  if (consume) {
    return 0;
  }
  return forward_to_host(hwnd, message, w_param, l_param);
}

}

bool Initialize(HWND hwnd, ID3D11Device *device,
                ID3D11DeviceContext *context) noexcept {
  if (hwnd == nullptr || device == nullptr || context == nullptr ||
      !IsWindow(hwnd)) {
    report_gui_error("gui::Initialize arguments", ERROR_INVALID_PARAMETER);
    return false;
  }

  DWORD process_id = 0;
  if (GetWindowThreadProcessId(hwnd, &process_id) == 0) {
    const DWORD error = GetLastError();
    report_gui_error("GetWindowThreadProcessId", error);
    return false;
  }
  if (process_id != GetCurrentProcessId()) {
    report_gui_error("gui::Initialize OutputWindow ownership",
                     ERROR_INVALID_WINDOW_HANDLE);
    return false;
  }

  phase expected = phase::uninitialized;
  if (!g_phase.compare_exchange_strong(expected, phase::initializing,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    return expected == phase::ready &&
           !g_shutting_down.load(std::memory_order_acquire);
  }

  g_shutting_down.store(false, std::memory_order_release);
  g_wndproc_was_installed.store(false, std::memory_order_release);
  g_wndproc_detached_safely.store(false, std::memory_order_release);

  if (!InitializeCriticalSectionEx(&g_imgui_lock, 4000, 0)) {
    const DWORD error = GetLastError();
    report_gui_error("InitializeCriticalSectionEx", error);
    g_phase.store(phase::failed, std::memory_order_release);
    return false;
  }
  g_lock_initialized.store(true, std::memory_order_release);

  bool initialized = false;
  try {
    critical_section_guard lock(g_imgui_lock);
    if (g_shutting_down.load(std::memory_order_acquire)) {
      report_gui_error("gui::Initialize interrupted by shutdown",
                       ERROR_OPERATION_ABORTED);
    } else {
      g_hwnd = hwnd;
      g_context = context;

      if (!IMGUI_CHECKVERSION()) {
        report_gui_error("IMGUI_CHECKVERSION", ERROR_REVISION_MISMATCH);
      } else if (ImGui::CreateContext() == nullptr) {
        report_gui_error("ImGui::CreateContext", ERROR_NOT_ENOUGH_MEMORY);
      } else {
        g_context_created = true;
        ImGui::StyleColorsDark();

        if (!ImGui_ImplWin32_Init(hwnd)) {
          report_gui_error("ImGui_ImplWin32_Init", ERROR_DLL_INIT_FAILED);
        } else {
          g_win32_initialized = true;
          if (!ImGui_ImplDX11_Init(device, context)) {
            report_gui_error("ImGui_ImplDX11_Init", ERROR_DLL_INIT_FAILED);
          } else {
            g_dx11_initialized = true;

            WNDPROC observed = nullptr;
            DWORD error = ERROR_SUCCESS;
            if (!read_wndproc(hwnd, observed, error)) {
              report_gui_error("GetWindowLongPtrW(install)", error);
            } else {
              g_previous_wndproc.store(observed, std::memory_order_release);
              SetLastError(ERROR_SUCCESS);
              const LONG_PTR displaced =
                  SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                    reinterpret_cast<LONG_PTR>(&HookedWndProc));
              error = GetLastError();
              if (displaced == 0 && error != ERROR_SUCCESS) {
                report_gui_error("SetWindowLongPtrW(install)", error);
              } else {
                const WNDPROC actual = reinterpret_cast<WNDPROC>(displaced);
                if (actual != nullptr && actual != observed) {
                  g_previous_wndproc.store(actual, std::memory_order_release);
                }
                g_wndproc_installed.store(true, std::memory_order_release);
                g_wndproc_was_installed.store(true, std::memory_order_release);

                if (actual != observed) {
                  report_gui_error("WndProc install: concurrent change",
                                   ERROR_BUSY);
                } else {
                  g_phase.store(phase::ready, std::memory_order_release);
                  initialized = true;
                }
              }
            }
          }
        }
      }
    }

    if (!initialized && !g_wndproc_installed.load(std::memory_order_acquire)) {
      destroy_imgui_locked();
      g_phase.store(phase::failed, std::memory_order_release);
    }
  } catch (...) {
    report_gui_error("gui::Initialize exception", ERROR_UNHANDLED_EXCEPTION);
    g_shutting_down.store(true, std::memory_order_release);
    g_phase.store(phase::failed, std::memory_order_release);
  }

  if (initialized) {
    return true;
  }

  if (g_wndproc_installed.load(std::memory_order_acquire)) {
    g_shutting_down.store(true, std::memory_order_release);
    if (!DetachWndProcAndDrain()) {
      return false;
    }

    critical_section_guard lock(g_imgui_lock);
    destroy_imgui_locked();
    g_phase.store(phase::failed, std::memory_order_release);
  } else {
    critical_section_guard lock(g_imgui_lock);
    destroy_imgui_locked();
    g_phase.store(phase::failed, std::memory_order_release);
  }

  if (g_lock_initialized.exchange(false, std::memory_order_acq_rel)) {
    DeleteCriticalSection(&g_imgui_lock);
  }
  return false;
}

void Render(ID3D11RenderTargetView *rtv) noexcept {
  if (rtv == nullptr || g_shutting_down.load(std::memory_order_acquire) ||
      g_phase.load(std::memory_order_acquire) != phase::ready ||
      !g_lock_initialized.load(std::memory_order_acquire)) {
    return;
  }

  try {
    critical_section_guard lock(g_imgui_lock);
    if (g_shutting_down.load(std::memory_order_acquire) ||
        g_phase.load(std::memory_order_acquire) != phase::ready ||
        !g_context_created || !g_win32_initialized || !g_dx11_initialized ||
        g_context == nullptr) {
      return;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGuiIO &io = ImGui::GetIO();
    io.MouseDrawCursor = g_menu_open;

    bool request_shutdown = false;
    if (g_menu_open) {
      ImGui::Begin("izanagi: telemetry overlay");
      const ImGuiViewport *viewport = ImGui::GetMainViewport();
      ImGui::Text("FPS: %.1f", io.Framerate);
      ImGui::Text("Viewport: %.0f x %.0f", viewport->Size.x, viewport->Size.y);
      ImGui::Text("HWND: %p", static_cast<void *>(g_hwnd));
      ImGui::TextUnformatted("Runtime: active");
      request_shutdown = ImGui::Button("Unload Module");
      ImGui::End();
    }

    ImGui::Render();

    {
      om_bindings_guard bindings(g_context);
      ID3D11RenderTargetView *target = rtv;
      g_context->OMSetRenderTargets(1, &target, nullptr);
      ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    if (request_shutdown) {
      runtime::RequestShutdown();
    }
  } catch (...) {
    report_gui_error("gui::Render exception", ERROR_UNHANDLED_EXCEPTION);
    g_shutting_down.store(true, std::memory_order_release);
    g_phase.store(phase::failed, std::memory_order_release);
    runtime::RequestShutdown();
  }
}

bool DetachWndProcAndDrain() noexcept {
  g_shutting_down.store(true, std::memory_order_release);
  g_phase.store(phase::shutting_down, std::memory_order_release);

  bool restored = true;
  const bool needs_barrier =
      g_wndproc_was_installed.load(std::memory_order_acquire) &&
      !g_wndproc_detached_safely.load(std::memory_order_acquire);
  HWND hwnd = nullptr;
  if (g_lock_initialized.load(std::memory_order_acquire)) {
    critical_section_guard lock(g_imgui_lock);
    hwnd = g_hwnd;
    restored = restore_wndproc_locked();
  } else if (g_wndproc_installed.load(std::memory_order_acquire)) {
    report_unsafe_wndproc("WndProc restore: ImGui lock unavailable",
                          ERROR_INVALID_STATE);
    restored = false;
  }

  if (!restored) {
    return false;
  }

  // sincroniza com a thread da janela antes de callbacks.
  if (needs_barrier && !synchronize_window_thread(hwnd)) {
    return false;
  }

  wait_for_wndproc_drain();
  g_wndproc_detached_safely.store(true, std::memory_order_release);
  return true;
}

void Shutdown() noexcept {
  if (!g_lock_initialized.load(std::memory_order_acquire)) {
    return;
  }

  if (!g_wndproc_detached_safely.load(std::memory_order_acquire) ||
      g_wndproc_installed.load(std::memory_order_acquire) ||
      g_active_wndproc_callbacks.load(std::memory_order_acquire) != 0) {
    report_unsafe_wndproc("gui::Shutdown before safe WndProc drain",
                          ERROR_BUSY);
    return;
  }

  {
    critical_section_guard lock(g_imgui_lock);
    destroy_imgui_locked();
    g_phase.store(phase::uninitialized, std::memory_order_release);
  }

  g_lock_initialized.store(false, std::memory_order_release);
  DeleteCriticalSection(&g_imgui_lock);
}

bool IsInitialized() noexcept {
  return g_phase.load(std::memory_order_acquire) == phase::ready &&
         !g_shutting_down.load(std::memory_order_acquire);
}

bool InGuiCall() noexcept { return g_imgui_lock_depth != 0; }

}
