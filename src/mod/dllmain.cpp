// Entry point of cdmp.asi. Loaded into CrimsonDesert.exe by Ultimate ASI
// Loader (installed as winmm.dll next to the game exe), or by cdmp_host.exe
// for testing without the game.

#include <windows.h>

#include <cwchar>

#include "mod/mod.h"

namespace {

// The loader is a winmm.dll proxy, so every process in bin64 that imports
// winmm loads us too, including the game's crashpad_handler.exe. Only run
// inside the game itself, or in a test host that opts in explicitly.
bool is_allowed_host() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    if (_wcsicmp(name, L"CrimsonDesert.exe") == 0) return true;
    return GetEnvironmentVariableW(L"CDMP_TEST_HOST", nullptr, 0) > 0;
}

bool g_started = false;

}  // namespace

// Lets a test host stop the worker cleanly before FreeLibrary.
extern "C" __declspec(dllexport) void cdmp_shutdown() {
    if (g_started) cdmp::stop();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (is_allowed_host()) {
            g_started = true;
            cdmp::start(module);
        }
    }
    // On DLL_PROCESS_DETACH we do nothing: during process exit our thread is
    // already gone, and waiting on it under the loader lock would deadlock.
    return TRUE;
}
