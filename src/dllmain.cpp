#include <Windows.h>

#include "diagnostics.hpp"
#include "runtime.hpp"

namespace {

constinit HMODULE g_module = nullptr;
constinit DWORD g_worker_handle_close_error = ERROR_SUCCESS;

DWORD preserve_first_error(DWORD current, DWORD candidate) noexcept
{
    return current == ERROR_SUCCESS ? candidate : current;
}

DWORD WINAPI worker_thread(LPVOID parameter) noexcept
{
    const HMODULE module = *static_cast<const HMODULE*>(parameter);

    DWORD exit_code = g_worker_handle_close_error;
    bool can_unload = true;
    if (exit_code != ERROR_SUCCESS) {
        izanagi::report_error("CloseHandle(worker thread)", exit_code);
    }

    {
        izanagi::runtime runtime;
        DWORD runtime_error = ERROR_SUCCESS;

        try {
            runtime_error = runtime.Initialize();
            if (runtime_error == ERROR_SUCCESS) {
                runtime.Run();
            }
        } catch (...) {
            runtime_error = preserve_first_error(runtime_error, ERROR_UNHANDLED_EXCEPTION);
            izanagi::report_error("runtime exception", ERROR_UNHANDLED_EXCEPTION);
        }

        const DWORD shutdown_error = runtime.Shutdown();
        can_unload = runtime.CanUnload();
        exit_code = preserve_first_error(exit_code, runtime_error);
        exit_code = preserve_first_error(exit_code, shutdown_error);
    }

    if (!can_unload) {
        return exit_code;
    }

    FreeLibraryAndExitThread(module, exit_code);
}

}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);

    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        g_worker_handle_close_error = ERROR_SUCCESS;

        HANDLE thread = CreateThread(nullptr, 0, &worker_thread, &g_module, 0, nullptr);
        if (thread == nullptr) {
            const DWORD error = GetLastError();
            SetLastError(error);
            return FALSE;
        }

        if (!CloseHandle(thread)) {
            g_worker_handle_close_error = GetLastError();
        }
    }


    return TRUE;
}
