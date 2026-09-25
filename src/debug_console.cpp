#include "debug_console.hpp"
#include "diagnostics.hpp"

#include <cerrno>
#include <iostream>
#include <utility>

#if !defined(_MSC_VER) || !defined(_M_X64) || defined(_DLL)
#error This implementation requires MSVC x64 and the static CRT (/MT or /MTd).
#endif

namespace izanagi {
namespace {

constexpr DWORD std_ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};

void remember_error(DWORD& first, DWORD error) noexcept {
    if (first == ERROR_SUCCESS) {
        first = error;
    }
}

void close_stream(FILE*& stream, const char* operation, DWORD& first) noexcept {
    if (stream == nullptr) {
        return;
    }
    // fclose invalida o stream mesmo quando retorna erro.
    FILE* const owned = std::exchange(stream, nullptr);
    if (std::fclose(owned) == EOF) {
        const int error = errno;
        report_error(operation, static_cast<unsigned long>(error));
        remember_error(first, ERROR_WRITE_FAULT);
    }
}

void flush_stream(FILE* stream, const char* operation, DWORD& first) noexcept {
    if (stream != nullptr && std::fflush(stream) == EOF) {
        const int error = errno;
        report_error(operation, static_cast<unsigned long>(error));
        remember_error(first, ERROR_WRITE_FAULT);
    }
}

template <typename Char>
void detach_stream(std::basic_ios<Char>& stream) noexcept {
    stream.exceptions(std::ios_base::goodbit);
    stream.tie(nullptr);
    stream.rdbuf(nullptr);
}

}

debug_console::~debug_console() noexcept {
    (void)Shutdown();
}

DWORD debug_console::Initialize() {
    if (std::exchange(attempted_, true)) {
        return ERROR_ALREADY_INITIALIZED;
    }

    if (!AllocConsole()) {
        const DWORD error = GetLastError();
        report_error("AllocConsole", error);
        return error;
    }
    allocated_ = true;
    streams_touched_ = true;

    errno_t error = freopen_s(&in_, "CONIN$", "r", stdin);
    if (error != 0) {
        report_error("freopen_s(stdin), errno", static_cast<unsigned long>(error));
        (void)Shutdown();
        return ERROR_OPEN_FAILED;
    }
    error = freopen_s(&out_, "CONOUT$", "w", stdout);
    if (error != 0) {
        report_error("freopen_s(stdout), errno", static_cast<unsigned long>(error));
        (void)Shutdown();
        return ERROR_OPEN_FAILED;
    }
    error = freopen_s(&err_, "CONOUT$", "w", stderr);
    if (error != 0) {
        report_error("freopen_s(stderr), errno", static_cast<unsigned long>(error));
        (void)Shutdown();
        return ERROR_OPEN_FAILED;
    }

    std::cin.clear();
    std::cout.clear();
    std::cerr.clear();
    std::clog.clear();
    return ERROR_SUCCESS;
}

DWORD debug_console::Shutdown() noexcept {
    DWORD first = ERROR_SUCCESS;
    if (streams_touched_) {
        try {
            if (out_ != nullptr) {
                std::cout.flush();
            }
            if (err_ != nullptr) {
                std::cerr.flush();
                std::clog.flush();
            }
            if ((out_ != nullptr && !std::cout) ||
                (err_ != nullptr && (!std::cerr || !std::clog))) {
                report_error("iostream flush", ERROR_WRITE_FAULT);
                remember_error(first, ERROR_WRITE_FAULT);
            }
        } catch (...) {
            report_error("iostream flush exception", ERROR_WRITE_FAULT);
            remember_error(first, ERROR_WRITE_FAULT);
        }

        detach_stream(std::cin);
        detach_stream(std::cout);
        detach_stream(std::cerr);
        detach_stream(std::clog);
        detach_stream(std::wcin);
        detach_stream(std::wcout);
        detach_stream(std::wcerr);
        detach_stream(std::wclog);
        streams_touched_ = false;

        flush_stream(err_, "fflush(stderr), errno", first);
        flush_stream(out_, "fflush(stdout), errno", first);
        close_stream(err_, "fclose(stderr), errno", first);
        close_stream(out_, "fclose(stdout), errno", first);
        close_stream(in_, "fclose(stdin), errno", first);
    }

    if (std::exchange(allocated_, false)) {
        if (!FreeConsole()) {
            const DWORD error = GetLastError();
            report_error("FreeConsole", error);
            remember_error(first, error);
        }
        for (const DWORD id : std_ids) {
            if (!SetStdHandle(id, nullptr)) {
                const DWORD error = GetLastError();
                report_error("SetStdHandle(restore)", error);
                remember_error(first, error);
            }
        }
    }
    return first;
}

}
