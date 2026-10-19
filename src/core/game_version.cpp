#include "core/game_version.h"

#include <windows.h>

#include <format>
#include <vector>

namespace ivc {

std::string GameVersion::str() const { return std::format("{}.{}.{}.{}", major, minor, build, revision); }

GameVersion query_game_version() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);

  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(path, &handle);
  if (!size) return {};
  std::vector<char> data(size);
  if (!GetFileVersionInfoW(path, 0, size, data.data())) return {};

  VS_FIXEDFILEINFO* info = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return {};
  return {HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
          LOWORD(info->dwFileVersionLS)};
}

} // namespace ivc
