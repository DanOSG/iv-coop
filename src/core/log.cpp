#include "core/log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>

namespace ivc::log {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
Level g_min = Level::Info;

const char* tag(Level l) {
  switch (l) {
    case Level::Debug: return "DBG";
    case Level::Info:  return "INF";
    case Level::Warn:  return "WRN";
    case Level::Error: return "ERR";
  }
  return "???";
}
} // namespace

void init(const std::filesystem::path& file) {
  std::lock_guard lock(g_mutex);
  if (g_file) return;
  // The last launch's log is kept as <name>.previous.log (a crash's log survives the restart that follows).
  std::filesystem::path previous = file;
  previous.replace_extension(L".previous.log");
  MoveFileExW(file.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
  g_file = _wfsopen(file.c_str(), L"w", _SH_DENYWR); // truncate each launch
}

void set_min_level(Level level) { g_min = level; }

void write(Level level, std::string_view msg) {
  if (level < g_min) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::lock_guard lock(g_mutex);
  if (!g_file) return;
  std::fprintf(g_file, "%02u:%02u:%02u.%03u [%s] %.*s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
               tag(level), static_cast<int>(msg.size()), msg.data());
  std::fflush(g_file);
}

} // namespace ivc::log
