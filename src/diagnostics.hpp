#pragma once

#include <Windows.h>
#include <cstdio>

namespace izanagi {

inline void report_error(const char* operation, unsigned long code) noexcept {
    char message[256]{};
    _snprintf_s(message, sizeof(message), _TRUNCATE,
                "[izanagi] %s: code=%lu\n", operation, code);
    OutputDebugStringA(message);
}

} // namespace izanagi
