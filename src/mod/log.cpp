#include "mod/log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>

namespace cdmp::log {
namespace {

std::mutex g_mutex;
HANDLE g_file = INVALID_HANDLE_VALUE;
// A session log never grows past this: a misbehaving (or malicious) peer
// must not be able to fill the disk through our log lines.
constexpr uint64_t kMaxLogBytes = 64ull * 1024 * 1024;
uint64_t g_written = 0;
bool g_full = false;

const char* level_tag(Level level) {
    switch (level) {
        case Level::Debug: return "DBG";
        case Level::Info: return "INF";
        case Level::Warn: return "WRN";
        case Level::Error: return "ERR";
    }
    return "???";
}

}  // namespace

bool open(const std::wstring& path) {
    std::lock_guard lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) return true;
    // One log per game session keeps reports small; the last sessions'
    // logs stay as cdmp.1.log (newest) ... cdmp.5.log, so a crash log is not
    // lost when the game is started again.
    const size_t dot = path.rfind(L'.');
    const std::wstring stem = dot == std::wstring::npos ? path : path.substr(0, dot);
    const std::wstring ext = dot == std::wstring::npos ? L"" : path.substr(dot);
    auto numbered = [&](int n) { return stem + L"." + std::to_wstring(n) + ext; };
    constexpr int kKept = 5;
    DeleteFileW(numbered(kKept).c_str());
    for (int n = kKept - 1; n >= 1; --n) MoveFileExW(numbered(n).c_str(), numbered(n + 1).c_str(), MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(path.c_str(), numbered(1).c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    g_written = 0;
    g_full = false;
    return g_file != INVALID_HANDLE_VALUE;
}

void close() {
    std::lock_guard lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void write(Level level, std::string_view text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char prefix[48];
    const int n = std::snprintf(prefix, sizeof prefix, "[%02d:%02d:%02d.%03d] %s [T%lu] ",
                                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                                level_tag(level), GetCurrentThreadId());
    std::string line(prefix, n > 0 ? static_cast<size_t>(n) : 0);
    line.append(text);
    line.append("\r\n");

    std::lock_guard lock(g_mutex);
    if (g_file == INVALID_HANDLE_VALUE || g_full) return;
    if (g_written + line.size() > kMaxLogBytes) {
        g_full = true;
        line = "[log full: nothing more is written this session]\r\n";
    }
    DWORD written = 0;
    WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    g_written += written;
    // The text is in the OS cache already (it survives the game crashing);
    // forcing it to disk on every line cost a disk write per line.
    if (level >= Level::Warn || g_full) FlushFileBuffers(g_file);
}

}  // namespace cdmp::log
