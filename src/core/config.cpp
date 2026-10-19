#include "core/config.h"

#include <windows.h>

#include <format>
#include <string>

namespace ivc {
namespace {

std::string narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s.push_back(static_cast<char>(c)); // ini values are ASCII
  return s;
}

std::string read_str(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const std::string& def) {
  constexpr wchar_t kMissing[] = L"\x01";
  wchar_t buf[512];
  GetPrivateProfileStringW(section, key, kMissing, buf, 512, ini.c_str());
  if (std::wstring(buf) == kMissing) {
    WritePrivateProfileStringW(section, key, std::wstring(def.begin(), def.end()).c_str(), ini.c_str());
    return def;
  }
  return narrow(buf);
}

int read_int(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int def) {
  try {
    return std::stoi(read_str(ini, section, key, std::to_string(def)), nullptr, 0); // accepts 0x.. too
  } catch (...) {
    return def;
  }
}

float read_float(const std::wstring& ini, const wchar_t* section, const wchar_t* key, float def) {
  try {
    return std::stof(read_str(ini, section, key, std::format("{}", def)));
  } catch (...) {
    return def;
  }
}

std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t end = s.find(',', start);
    if (end == std::string::npos) end = s.size();
    std::string item = s.substr(start, end - start);
    item.erase(0, item.find_first_not_of(' '));
    item.erase(item.find_last_not_of(' ') + 1);
    if (!item.empty()) out.push_back(item);
    start = end + 1;
  }
  return out;
}

} // namespace

Config load_config(const std::filesystem::path& ini) {
  const std::wstring p = ini.wstring();
  Config c;
  c.overlay_enabled = read_int(p, L"Overlay", L"Enabled", c.overlay_enabled) != 0;
  c.overlay_toggle_key = read_int(p, L"Overlay", L"ToggleKey", c.overlay_toggle_key);
  c.scripts_panel_key = read_int(p, L"Overlay", L"ScriptsPanelKey", c.scripts_panel_key);
  c.debug_log = read_int(p, L"Log", L"Debug", c.debug_log) != 0;
  c.isolate_named_objects = read_int(p, L"Instance", L"IsolateNamedObjects", c.isolate_named_objects) != 0;

  c.host_scripts = split_csv(read_str(p, L"Scripts", L"TickHost", "main"));
  c.suppressed_scripts = split_csv(read_str(p, L"Scripts", L"Suppress", ""));

  auto& g = c.ghost;
  g.model = read_str(p, L"Ghost", L"Model", g.model);
  g.toggle_key = read_int(p, L"Ghost", L"ToggleKey", g.toggle_key);
  g.send_hz = static_cast<std::uint32_t>(read_int(p, L"Ghost", L"SendHz", static_cast<int>(g.send_hz)));
  g.latency_ms = static_cast<std::uint32_t>(read_int(p, L"Ghost", L"LatencyMs", static_cast<int>(g.latency_ms)));
  g.jitter_ms = static_cast<std::uint32_t>(read_int(p, L"Ghost", L"JitterMs", static_cast<int>(g.jitter_ms)));
  g.loss = read_float(p, L"Ghost", L"Loss", g.loss);
  g.interp_delay_ms =
      static_cast<std::uint32_t>(read_int(p, L"Ghost", L"InterpDelayMs", static_cast<int>(g.interp_delay_ms)));
  g.offset_x = read_float(p, L"Ghost", L"OffsetX", g.offset_x);
  g.animated = read_int(p, L"Ghost", L"Animated", g.animated) != 0;
  if (g.send_hz == 0) g.send_hz = 1;

  auto& n = c.net;
  n.name = read_str(p, L"Net", L"Name", n.name);
  n.join_address = read_str(p, L"Net", L"JoinAddress", n.join_address);
  n.port = static_cast<std::uint16_t>(read_int(p, L"Net", L"Port", n.port));
  n.bind_address = read_str(p, L"Net", L"BindAddress", n.bind_address);
  n.send_hz = static_cast<std::uint32_t>(read_int(p, L"Net", L"SendHz", static_cast<int>(n.send_hz)));
  n.interp_delay_ms =
      static_cast<std::uint32_t>(read_int(p, L"Net", L"InterpDelayMs", static_cast<int>(n.interp_delay_ms)));
  n.puppet_model = read_str(p, L"Net", L"PuppetModel", n.puppet_model);
  n.animated_puppets = read_int(p, L"Net", L"AnimatedPuppets", n.animated_puppets) != 0;
  n.host_key = read_int(p, L"Net", L"HostKey", n.host_key);
  n.join_key = read_int(p, L"Net", L"JoinKey", n.join_key);
  if (n.send_hz == 0) n.send_hz = 1;
  return c;
}

} // namespace ivc
