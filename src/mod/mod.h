#pragma once

#include <windows.h>

namespace cdmp {

// Called from DllMain (attach). Only spawns the worker thread; all real work
// happens there, outside the loader lock.
void start(HMODULE self);

// Signals the worker thread and waits for it. Never call under the loader
// lock (i.e. not from DllMain); the test host calls it via cdmp_shutdown()
// before FreeLibrary. Inside the game the process just exits.
void stop();

}  // namespace cdmp
