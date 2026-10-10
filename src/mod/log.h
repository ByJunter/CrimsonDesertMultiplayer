#pragma once
// Tiny thread-safe file logger. Every line is flushed immediately so the log
// survives a game crash, which is exactly when we need it most.

#include <format>
#include <string>
#include <string_view>

namespace cdmp::log {

enum class Level { Debug, Info, Warn, Error };

// Opens a new log; the previous ones become <name>.1<ext> ... <name>.5<ext>.
bool open(const std::wstring& path);
void close();
void write(Level level, std::string_view text);

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace cdmp::log
