#pragma once
#include <filesystem>
#include <format>
#include <string_view>

namespace ivc::log {

enum class Level { Debug, Info, Warn, Error };

void init(const std::filesystem::path& file);
void set_min_level(Level level);
void write(Level level, std::string_view msg);

template <class... Args>
void debug(std::format_string<Args...> f, Args&&... a) { write(Level::Debug, std::format(f, std::forward<Args>(a)...)); }
template <class... Args>
void info(std::format_string<Args...> f, Args&&... a) { write(Level::Info, std::format(f, std::forward<Args>(a)...)); }
template <class... Args>
void warn(std::format_string<Args...> f, Args&&... a) { write(Level::Warn, std::format(f, std::forward<Args>(a)...)); }
template <class... Args>
void error(std::format_string<Args...> f, Args&&... a) { write(Level::Error, std::format(f, std::forward<Args>(a)...)); }

} // namespace ivc::log
