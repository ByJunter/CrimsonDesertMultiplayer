#pragma once
// Reads of game memory from the mod's worker thread. The game frees and
// reallocates objects on its own thread (loading screens, fast travel), so
// every pointer we follow may be stale. Each read checks that the pages are
// committed and readable, and is additionally wrapped in SEH so a race that
// unmaps memory between the check and the copy cannot crash the game.

#include <cstddef>
#include <cstdint>
#include <optional>

namespace cdmp::game {

bool safe_read(uintptr_t addr, void* out, size_t size);

template <class T>
std::optional<T> read(uintptr_t addr) {
    T v{};
    if (!safe_read(addr, &v, sizeof v)) return std::nullopt;
    return v;
}

// Reads a pointer at addr and checks it is a plausible user-mode address.
std::optional<uintptr_t> read_ptr(uintptr_t addr);

// True if the object at `obj` starts with the expected vtable pointer.
bool has_vtable(uintptr_t obj, uintptr_t expected_vtable);

}  // namespace cdmp::game
