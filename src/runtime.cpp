#include "runtime.hpp"

#include "diagnostics.hpp"
#include "dx11_hook.hpp"
#include "runtime_services.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>

namespace izanagi {

namespace {

constexpr DWORD k_stream_error = ERROR_WRITE_FAULT;
constinit std::atomic<bool> g_shutdown_requested{false};

DWORD preserve_first_error(DWORD current, DWORD candidate) noexcept
{
    return current == ERROR_SUCCESS ? candidate : current;
}

}

runtime::~runtime() noexcept
{
    (void)Shutdown();
}

void runtime::RequestShutdown() noexcept
{
    g_shutdown_requested.store(true, std::memory_order_release);
}

DWORD runtime::Initialize()
{
    if (state_ != lifecycle::cold) {
        return ERROR_ALREADY_INITIALIZED;
    }

    g_shutdown_requested.store(false, std::memory_order_release);

    const DWORD console_error = console_.Initialize();
    if (console_error != ERROR_SUCCESS) {
        report_error("debug_console::Initialize", console_error);
        return console_error;
    }

    std::cout << "izanagi: runtime initialized\n" << std::flush;
    if (!std::cout.good()) {
        report_error("runtime initialization output", k_stream_error);
        return k_stream_error;
    }

    const auto services_started = std::chrono::steady_clock::now();
    if (!runtime_services::Initialize()) {
        report_error("runtime_services::Initialize", ERROR_GEN_FAILURE);
        return ERROR_GEN_FAILURE;
    }
    const auto services_ready = std::chrono::steady_clock::now();
    hook_attempted_ = true;
    if (!dx11_hook::Initialize()) {
        report_error("dx11_hook::Initialize", ERROR_GEN_FAILURE);
        return ERROR_GEN_FAILURE;
    }
    const auto hook_ready = std::chrono::steady_clock::now();
    const auto millis = [](auto from, auto to) noexcept {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    (void)std::fprintf(stdout, "izanagi: startup timing services=%.1f ms hook=%.1f ms\n",
                       millis(services_started, services_ready),
                       millis(services_ready, hook_ready));

    state_ = lifecycle::active;
    return ERROR_SUCCESS;
}

void runtime::Run()
{
    if (state_ != lifecycle::active) {
        return;
    }


    bool end_was_down = (GetAsyncKeyState(VK_END) & 0x8000) != 0;

    auto next_refresh = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        runtime_services::PollServices();
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_refresh) {
            runtime_services::RefreshModules();
            next_refresh = now + std::chrono::seconds(5);
        }
        const bool end_is_down = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        if (end_is_down && !end_was_down) {
            RequestShutdown();
        }

        end_was_down = end_is_down;
        if (!g_shutdown_requested.load(std::memory_order_acquire)) {
            Sleep(100);
        }
    }
}

DWORD runtime::Shutdown() noexcept
{
    if (state_ == lifecycle::stopped) {
        return ERROR_SUCCESS;
    }

    DWORD first_error = ERROR_SUCCESS;

    if (state_ == lifecycle::active) {
        try {
            std::cout << "izanagi: runtime shutting down\n" << std::flush;
        } catch (...) {
            first_error = k_stream_error;
            report_error("runtime shutdown output", k_stream_error);
        }

        if (first_error == ERROR_SUCCESS && !std::cout.good()) {
            first_error = k_stream_error;
            report_error("runtime shutdown output", k_stream_error);
        }
    }

    runtime_services::BeginShutdown();
    bool hooks_drained = true;
    if (hook_attempted_) {
        if (!dx11_hook::Shutdown()) {
            hooks_drained = false;
            can_unload_ = false;
            first_error = preserve_first_error(first_error, ERROR_BUSY);
            report_error("dx11_hook::Shutdown: module pinned", ERROR_BUSY);
        }
        hook_attempted_ = false;
    }
    if (hooks_drained) {
        runtime_services::Shutdown();
    }

    const DWORD console_error = console_.Shutdown();
    state_ = lifecycle::stopped;

    if (console_error != ERROR_SUCCESS) {
        report_error("debug_console::Shutdown", console_error);
        first_error = preserve_first_error(first_error, console_error);
    }

    return first_error;
}

}
