#include "mod/game/rtti.h"

#include <windows.h>

#include <cstring>

#include "mod/game/safe_memory.h"

namespace cdmp::game {

std::string rtti_name_of_vtable(uintptr_t vtable) {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    // x64 CompleteObjectLocator: u32 signature (1), ..., u32 TypeDescriptor RVA at +0x0C.
    auto col = read_ptr(vtable - 8);
    if (!col) return {};
    auto signature = read<uint32_t>(*col);
    auto td_rva = read<uint32_t>(*col + 0x0C);
    if (!signature || *signature != 1 || !td_rva) return {};
    char name[160] = {};
    if (!safe_read(base + *td_rva + 0x10, name, sizeof name - 1)) return {};
    if (std::strncmp(name, ".?A", 3) != 0) return {};
    return name;
}

std::string rtti_name_of_object(uintptr_t obj) {
    auto vtable = read_ptr(obj);
    return vtable ? rtti_name_of_vtable(*vtable) : std::string{};
}

std::string short_class_name(const std::string& decorated, const char* strip_prefix) {
    // ".?AV" / ".?AU" + Name + "@" + namespaces + "@@"
    if (decorated.size() < 6 || decorated.compare(0, 3, ".?A") != 0) return decorated;
    std::string name = decorated.substr(4, decorated.find('@', 4) - 4);
    const size_t prefix_len = std::strlen(strip_prefix);
    if (prefix_len && name.compare(0, prefix_len, strip_prefix) == 0) name = name.substr(prefix_len);
    return name;
}

}  // namespace cdmp::game
