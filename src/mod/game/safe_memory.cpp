#include "mod/game/safe_memory.h"

#include <windows.h>

#include <cstring>

namespace cdmp::game {
namespace {

constexpr uintptr_t kMinAddress = 0x10000;
constexpr uintptr_t kMaxAddress = 0x00007FFFFFFFFFFFull;

bool pages_readable(uintptr_t addr, size_t size) {
    uintptr_t cur = addr;
    const uintptr_t end = addr + size;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof mbi) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        cur = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

// Kept free of C++ objects with destructors so __try is allowed here.
bool guarded_copy(void* out, const void* src, size_t size) {
    __try {
        std::memcpy(out, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool safe_read(uintptr_t addr, void* out, size_t size) {
    if (addr < kMinAddress || addr > kMaxAddress - size) return false;
    if (!pages_readable(addr, size)) return false;
    return guarded_copy(out, reinterpret_cast<const void*>(addr), size);
}

std::optional<uintptr_t> read_ptr(uintptr_t addr) {
    auto p = read<uintptr_t>(addr);
    if (!p || *p < kMinAddress || *p > kMaxAddress) return std::nullopt;
    return p;
}

bool has_vtable(uintptr_t obj, uintptr_t expected_vtable) {
    auto vt = read<uintptr_t>(obj);
    return vt && *vt == expected_vtable;
}

}  // namespace cdmp::game
