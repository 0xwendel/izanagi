#include "runtime.hpp"

#include "diagnostics.hpp"

#include <iostream>

namespace izanagi {

namespace {

constexpr DWORD k_stream_error = ERROR_WRITE_FAULT;

DWORD preserve_first_error(DWORD current, DWORD candidate) noexcept
{
    return current == ERROR_SUCCESS ? candidate : current;
}

} // namespace

runtime::~runtime() noexcept
{
    (void)Shutdown();
}

DWORD runtime::Initialize()
{
    if (state_ != lifecycle::cold) {
        return ERROR_ALREADY_INITIALIZED;
    }

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

    state_ = lifecycle::active;
    return ERROR_SUCCESS;
}

void runtime::Run()
{
    if (state_ != lifecycle::active) {
        return;
    }


    bool end_was_down = (GetAsyncKeyState(VK_END) & 0x8000) != 0;

    for (;;) {
        const bool end_is_down = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        if (end_is_down && !end_was_down) {
            break;
        }

        end_was_down = end_is_down;
        Sleep(100);
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

    const DWORD console_error = console_.Shutdown();
    state_ = lifecycle::stopped;

    if (console_error != ERROR_SUCCESS) {
        report_error("debug_console::Shutdown", console_error);
        first_error = preserve_first_error(first_error, console_error);
    }

    return first_error;
}

} // namespace izanagi
