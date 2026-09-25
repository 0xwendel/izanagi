#pragma once

#include <Windows.h>
#include <cstdio>

namespace izanagi {

class debug_console final {
public:
    debug_console() noexcept = default;
    ~debug_console() noexcept;

    debug_console(const debug_console&) = delete;
    debug_console& operator=(const debug_console&) = delete;
    debug_console(debug_console&&) = delete;
    debug_console& operator=(debug_console&&) = delete;

    [[nodiscard]] DWORD Initialize();
    [[nodiscard]] DWORD Shutdown() noexcept;

private:
    FILE* in_ = nullptr;
    FILE* out_ = nullptr;
    FILE* err_ = nullptr;
    bool attempted_ = false;
    bool allocated_ = false;
    bool streams_touched_ = false;
};

}
