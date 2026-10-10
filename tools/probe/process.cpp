#include "probe/process.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <format>

namespace probe {

Process::~Process() {
    if (handle_) CloseHandle(handle_);
}

bool Process::attach_by_name(const std::wstring& exe_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    uint32_t found = 0;
    for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
        if (_wcsicmp(entry.szExeFile, exe_name.c_str()) == 0) {
            found = entry.th32ProcessID;
            break;
        }
    }
    CloseHandle(snap);
    return found != 0 && attach_by_pid(found);
}

bool Process::attach_by_pid(uint32_t pid) {
    // Read + query only: the probe must never be able to modify the game.
    handle_ = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!handle_) return false;
    pid_ = pid;
    return load_main_module();
}

bool Process::load_main_module() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid_);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W mod{};
    mod.dwSize = sizeof mod;
    const bool ok = Module32FirstW(snap, &mod) != FALSE;  // first entry is the exe
    CloseHandle(snap);
    if (!ok) return false;
    base_ = reinterpret_cast<uint64_t>(mod.modBaseAddr);
    size_ = mod.modBaseSize;
    path_ = mod.szExePath;
    return true;
}

bool Process::read(uint64_t addr, void* out, size_t size) const {
    SIZE_T got = 0;
    return ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(addr), out, size, &got) &&
           got == size;
}

std::string Process::read_cstring(uint64_t addr, size_t max_len) const {
    std::string s;
    char buf[64];
    while (s.size() < max_len) {
        if (!read(addr + s.size(), buf, sizeof buf)) break;
        for (char c : buf) {
            if (c == '\0' || s.size() >= max_len) return s;
            s.push_back(c);
        }
    }
    return s;
}

bool Process::is_readable(uint64_t addr) const {
    if (addr < 0x10000 || addr > 0x00007FFFFFFFFFFFull) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQueryEx(handle_, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return true;
}

std::vector<uint64_t> Process::scan(const cdmp::Pattern& pattern, size_t max_matches,
                                    Scope scope) const {
    std::vector<uint64_t> result;
    std::vector<size_t> offsets;
    std::vector<uint8_t> buf;
    constexpr size_t kChunk = 16 * 1024 * 1024;
    const size_t overlap = pattern.size() - 1;
    constexpr DWORD kWritable =
        PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    uint64_t addr = scope == Scope::Module ? base_ : 0x10000;
    const uint64_t end = scope == Scope::Module ? base_ + size_ : 0x00007FFFFFFF0000ull;
    while (addr < end && result.size() < max_matches) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(handle_, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi)) break;
        const uint64_t region_start = reinterpret_cast<uint64_t>(mbi.BaseAddress);
        const uint64_t region_end = std::min<uint64_t>(region_start + mbi.RegionSize, end);
        bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (scope == Scope::Heap) {
            readable = readable && mbi.Type != MEM_IMAGE && (mbi.Protect & kWritable);
        }

        if (readable) {
            for (uint64_t pos = std::max(addr, region_start); pos < region_end;) {
                const size_t want = static_cast<size_t>(std::min<uint64_t>(kChunk, region_end - pos));
                buf.resize(want);
                SIZE_T got = 0;
                ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(pos), buf.data(), want, &got);
                if (got == 0) break;
                offsets.clear();
                cdmp::find_pattern({buf.data(), got}, pattern, offsets,
                                   max_matches - result.size());
                for (size_t off : offsets) result.push_back(pos + off);
                if (result.size() >= max_matches || got <= overlap) break;
                // Step back by the pattern length so matches spanning two
                // chunks are not missed (and not reported twice).
                pos += got - overlap;
                if (got < want) break;
            }
        }
        addr = region_end;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::string Process::rtti_name(uint64_t obj) const {
    auto vtable = read<uint64_t>(obj);
    if (!vtable || !in_module(*vtable)) return {};
    auto col = read<uint64_t>(*vtable - 8);
    if (!col || !in_module(*col)) return {};
    // x64 CompleteObjectLocator: signature 1, then RVA of TypeDescriptor at +0x0C.
    auto signature = read<uint32_t>(*col);
    auto td_rva = read<uint32_t>(*col + 0x0C);
    if (!signature || *signature != 1 || !td_rva) return {};
    std::string name = read_cstring(base_ + *td_rva + 0x10, 200);
    return name.starts_with(".?A") ? name : std::string{};
}

std::string Process::describe(uint64_t addr) const {
    if (in_module(addr)) return std::format("exe+0x{:X}", addr - base_);
    return std::format("0x{:X}", addr);
}

}  // namespace probe
