#include "sync/session_settings.h"

#include <windows.h>

#include <format>

#include "core/log.h"
#include "net/bytes.h"

namespace ivc::session_settings {
namespace {

constexpr DWORD kSendMs = 5000;

Settings g_own;             // ours (menu, ini)
Settings g_host;            // the host's, while joined
bool g_joined = false;
bool g_have_host = false;
std::wstring g_ini;
// Host: what was last sent.
Settings g_sent;
DWORD g_last_send = 0;
bool g_force_send = true;

const char* ff_name(FriendlyFire f) {
  switch (f) {
    case FriendlyFire::On: return "On";
    case FriendlyFire::DamageOnly: return "No aim";
    default: return "Off";
  }
}

void save() {
  if (g_ini.empty()) return;
  auto put = [](const wchar_t* key, int v) {
    WritePrivateProfileStringW(L"Session", key, std::to_wstring(v).c_str(), g_ini.c_str());
  };
  put(L"FriendlyFire", static_cast<int>(g_own.friendly_fire));
  put(L"Downed", g_own.downed);
  put(L"SharedPayouts", g_own.shared_payouts);
  put(L"RadioSync", g_own.radio_sync);
  put(L"WorldSync", g_own.world_sync);
  put(L"PartnerBlips", g_own.partner_blips);
  put(L"ScenesWatch", g_own.scenes_watch);
}

std::uint8_t pack_flags(const Settings& s) {
  return static_cast<std::uint8_t>(int{s.downed} | int{s.shared_payouts} << 1 | int{s.radio_sync} << 2 |
                                   int{s.world_sync} << 3 | int{s.partner_blips} << 4 | int{s.scenes_watch} << 5);
}

// The menu's settings, in order: name and number of values.
struct Entry {
  const char* name;
  int values;
};
// Labels fit one phone line (about 19 characters with the value).
constexpr Entry kEntries[] = {{"Team fire", 3},  {"Revive", 2},       {"Shared pay", 2},
                              {"Radio sync", 2}, {"Weather sync", 2}, {"Partner blips", 2}, {"Scenes", 2}};

int value_of(const Settings& s, int i) {
  switch (i) {
    case 0: return static_cast<int>(s.friendly_fire);
    case 1: return s.downed;
    case 2: return s.shared_payouts;
    case 3: return s.radio_sync;
    case 4: return s.world_sync;
    case 5: return s.partner_blips;
    default: return s.scenes_watch;
  }
}

void set_value(Settings& s, int i, int v) {
  switch (i) {
    case 0: s.friendly_fire = static_cast<FriendlyFire>(v); break;
    case 1: s.downed = v != 0; break;
    case 2: s.shared_payouts = v != 0; break;
    case 3: s.radio_sync = v != 0; break;
    case 4: s.world_sync = v != 0; break;
    case 5: s.partner_blips = v != 0; break;
    default: s.scenes_watch = v != 0; break;
  }
}

std::string value_text(int i, int v) {
  if (i == 0) return ff_name(static_cast<FriendlyFire>(v));
  if (i == 6) return v ? "Watch" : "Keep ctrl";
  return v ? "On" : "Off";
}

} // namespace

const Settings& current() { return g_joined && g_have_host ? g_host : g_own; }

void load(const std::filesystem::path& ini) {
  g_ini = ini.wstring();
  auto get = [](const wchar_t* key, int def) {
    return static_cast<int>(GetPrivateProfileIntW(L"Session", key, def, g_ini.c_str()));
  };
  const Settings d;
  const int ff = get(L"FriendlyFire", static_cast<int>(d.friendly_fire));
  g_own.friendly_fire = ff >= 0 && ff <= 2 ? static_cast<FriendlyFire>(ff) : d.friendly_fire;
  g_own.downed = get(L"Downed", d.downed) != 0;
  g_own.shared_payouts = get(L"SharedPayouts", d.shared_payouts) != 0;
  g_own.radio_sync = get(L"RadioSync", d.radio_sync) != 0;
  g_own.world_sync = get(L"WorldSync", d.world_sync) != 0;
  g_own.partner_blips = get(L"PartnerBlips", d.partner_blips) != 0;
  g_own.scenes_watch = get(L"ScenesWatch", d.scenes_watch) != 0;
  save(); // writes defaults so the section is there to edit
}

int count() { return static_cast<int>(std::size(kEntries)); }

std::string label(int i) { return std::format("{}: {}", kEntries[i].name, value_text(i, value_of(current(), i))); }

std::string label_key(int i) { return std::format("IVC_S{}_{}", i, value_of(current(), i)); }

void all_labels(void (*add)(const std::string& key, const std::string& text)) {
  for (int i = 0; i < count(); ++i)
    for (int v = 0; v < kEntries[i].values; ++v)
      add(std::format("IVC_S{}_{}", i, v), std::format("{}: {}", kEntries[i].name, value_text(i, v)));
}

void cycle(int i) {
  if (i < 0 || i >= count()) return;
  set_value(g_own, i, (value_of(g_own, i) + 1) % kEntries[i].values);
  save();
  g_force_send = true;
  log::info("Settings: {}", label(i));
}

void tick(bool in_session, bool hosting, net::Session& net) {
  g_joined = in_session && !hosting;
  if (!in_session) {
    g_have_host = false;
    g_force_send = true;
    return;
  }
  if (!hosting) return;
  const DWORD now = GetTickCount();
  if (!g_force_send && g_own == g_sent && now - g_last_send < kSendMs) return;
  g_force_send = false, g_sent = g_own, g_last_send = now;
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(static_cast<std::uint8_t>(g_own.friendly_fire)).put(pack_flags(g_own));
  net.send_game(std::move(w.data()), true);
}

void on_message(const net::GameMessage& m) {
  if (m.from != net::kHostId) return;
  net::Reader r(m.data);
  std::uint8_t type, sender, ff, flags;
  if (!r.get(type) || !r.get(sender) || !r.get(ff) || !r.get(flags) || ff > 2) return;
  Settings s;
  s.friendly_fire = static_cast<FriendlyFire>(ff);
  s.downed = flags & 1, s.shared_payouts = flags & 2, s.radio_sync = flags & 4, s.world_sync = flags & 8,
  s.partner_blips = flags & 16, s.scenes_watch = flags & 32;
  if (!g_have_host || !(s == g_host)) log::info("Settings: the host's: friendly fire {}, flags {:#x}", ff_name(s.friendly_fire), flags);
  g_host = s, g_have_host = true;
}

} // namespace ivc::session_settings
