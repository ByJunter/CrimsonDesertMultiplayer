#pragma once
// Read-only access to another process (the game) via ReadProcessMemory.
// Nothing here can write to the target.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/pattern.h"

namespace probe {

class Process {
public:
    ~Process();

    bool attach_by_name(const std::wstring& exe_name);
    bool attach_by_pid(uint32_t pid);

    uint32_t pid() const { return pid_; }
    uint64_t module_base() const { return base_; }
    uint64_t module_size() const { return size_; }
    const std::wstring& module_path() const { return path_; }
    bool in_module(uint64_t addr) const { return addr >= base_ && addr < base_ + size_; }

    bool read(uint64_t addr, void* out, size_t size) const;
    template <class T>
    std::optional<T> read(uint64_t addr) const {
        T v{};
        if (!read(addr, &v, sizeof v)) return std::nullopt;
        return v;
    }
    std::string read_cstring(uint64_t addr, size_t max_len = 256) const;

    // Is this a plausible user-mode pointer to committed readable memory?
    bool is_readable(uint64_t addr) const;

    enum class Scope {
        Module,  // the exe image (code + static data)
        Heap,    // committed writable memory outside any image
    };

    // Scans readable memory in `scope`. Returns sorted absolute addresses.
    std::vector<uint64_t> scan(const cdmp::Pattern& pattern, size_t max_matches,
                               Scope scope = Scope::Module) const;

    // MSVC RTTI class name of the object at `obj` (".?AVName@ns@@"), via
    // vtable[-1] -> CompleteObjectLocator -> TypeDescriptor. Empty if none.
    std::string rtti_name(uint64_t obj) const;

    // "exe+0x1234" when inside the main module, else "0x7FF...".
    std::string describe(uint64_t addr) const;

private:
    bool load_main_module();

    void* handle_ = nullptr;
    uint32_t pid_ = 0;
    uint64_t base_ = 0;
    uint64_t size_ = 0;
    std::wstring path_;
};

}  // namespace probe
