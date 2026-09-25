#pragma once

#if !defined(_MSC_VER)
#    error "izanagi requires the Microsoft C++ compiler"
#endif

#if !defined(_M_X64)
#    error "izanagi requires an x64 MSVC target"
#endif

#if defined(_DLL)
#    error "izanagi must use the static MSVC runtime (/MT or /MTd)"
#endif

#include <Windows.h>

#include "debug_console.hpp"

namespace izanagi {

class runtime final {
public:
    runtime() noexcept = default;
    ~runtime() noexcept;

    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;
    runtime(runtime&&) = delete;
    runtime& operator=(runtime&&) = delete;

    DWORD Initialize();
    void Run();
    DWORD Shutdown() noexcept;
    [[nodiscard]] bool CanUnload() const noexcept { return can_unload_; }

    static void RequestShutdown() noexcept;

private:
    enum class lifecycle : unsigned char {
        cold,
        active,
        stopped,
    };

    debug_console console_{};
    lifecycle state_{lifecycle::cold};
    bool hook_attempted_ = false;
    bool can_unload_ = true;
};

}
