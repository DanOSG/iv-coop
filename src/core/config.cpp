#include "core/config.h"

#include <windows.h>

#include <string>

namespace ivc {
namespace {
int read_int(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int def) {
  constexpr int kMissing = INT_MIN;
  int v = static_cast<int>(GetPrivateProfileIntW(section, key, kMissing, ini.c_str()));
  if (v == kMissing) {
    WritePrivateProfileStringW(section, key, std::to_wstring(def).c_str(), ini.c_str());
    return def;
  }
  return v;
}
} // namespace

Config load_config(const std::filesystem::path& ini) {
  const std::wstring p = ini.wstring();
  Config c;
  c.overlay_enabled = read_int(p, L"Overlay", L"Enabled", c.overlay_enabled) != 0;
  c.overlay_toggle_key = read_int(p, L"Overlay", L"ToggleKey", c.overlay_toggle_key);
  c.debug_log = read_int(p, L"Log", L"Debug", c.debug_log) != 0;
  return c;
}

} // namespace ivc
