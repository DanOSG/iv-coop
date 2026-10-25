#include "game/radio_probe.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>

#include "core/input.h"
#include "core/log.h"
#include "game/natives.h"

namespace ivc::radio_probe {
namespace {
namespace n = natives;
using n::invoke;

// Engine internals (static VAs, GTAIV.exe 1.2.0.59; docs/research-radio.md section 1).
constexpr std::uintptr_t kStationArray = 0x011D76AC; // station* []
constexpr std::uintptr_t kStationCount = 0x011D74F1; // byte
constexpr std::uintptr_t kMetadata = 0x0115D9A0;     // metadata store; lookup(name) -> object
constexpr std::uintptr_t kFnLookup = 0x00894EC0;     // thiscall(store, const char*) -> object*
constexpr std::uint32_t kSlotSize = 0xBD0, kOffCurSlot = 0x1917, kOffFrozen = 0x1927, kOffCurTrack = 0x190C;
constexpr std::uint32_t kSlotCategory = 0xBC0, kSlotTrack = 0xBC4, kSlotList = 0x960; // what ForceTrack sets

constexpr std::uint32_t kGetStationIndex = 0x4E493AAF, kGetRadioName = 0x7EC9580E, kIsRetuning = 0x45C344AA,
                        kFreeze = 0x08A015CF, kUnfreeze = 0x3E5B7E59, kForceTrack = 0x6A7E47C9;

template <class T> T at_static(std::uintptr_t va) {
  return reinterpret_cast<T>(va - 0x400000 + reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}

struct Playing {
  std::uint8_t slot = 0xFF, category = 0, frozen = 0;
  std::uint32_t track = ~0u, list = 0, cur_track = ~0u;
  bool operator==(const Playing&) const = default;
};

int station_count() {
  __try {
    return *at_static<std::uint8_t*>(kStationCount);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

bool read_station(int i, Playing& out) {
  __try {
    if (i >= *at_static<std::uint8_t*>(kStationCount)) return false;
    auto* st = (*at_static<std::uint8_t***>(kStationArray))[i];
    if (!st) return false;
    out.slot = st[kOffCurSlot];
    out.frozen = st[kOffFrozen];
    out.cur_track = *reinterpret_cast<std::uint32_t*>(st + kOffCurTrack);
    if (out.slot > 1) return true;
    const std::uint8_t* slot = st + out.slot * kSlotSize;
    out.category = slot[kSlotCategory];
    out.track = *reinterpret_cast<const std::uint32_t*>(slot + kSlotTrack);
    out.list = *reinterpret_cast<const std::uint32_t*>(slot + kSlotList);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

std::uint32_t lookup_list(const char* name) {
  __try {
    using Fn = std::uint8_t*(__thiscall*)(void*, const char*);
    std::uint8_t* obj = at_static<Fn>(kFnLookup)(at_static<void*>(kMetadata), name);
    return obj ? static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(obj + 0x40)) : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

bool g_enabled = false;
DWORD g_last = 0;
std::array<Playing, 32> g_seen{};
std::unordered_map<std::uint32_t, std::string> g_list_names; // entry+0x40 pointer -> list name
bool g_names_built = false;
bool g_k1 = true, g_k2 = true, g_k3 = true;

std::string station_name(int i) {
  const char* s = invoke<const char*>(kGetRadioName, i);
  return s ? s : "";
}

void build_names(int count) {
  static constexpr const char* kPatterns[] = {"RADIO_{}_MUSIC", "RADIO_{}_IDENTS", "RADIO_{}_DJSOLO",
                                              "RADIO_STATION_{}", "RADIO_STATION_{}_MUSIC", "RADIO_STATION_{}_INTRO_TRACK",
                                              "EP1_RADIO_{}_MUSIC", "EP2_RADIO_{}_MUSIC"};
  static constexpr const char* kShared[] = {"RADIO_ADVERTS", "RADIO_NEWS_01", "RADIO_NEWS_02", "RADIO_NEWS_NEWS",
                                            "RADIO_WEATHER_WEATHER", "RADIO_STATION_ADVERTS_GENERIC",
                                            "RADIO_STATION_ADVERTS_RUSSIAN", "RADIO_STATION_ADVERTS_SPANISH"};
  std::string found;
  auto add = [&](const std::string& name) {
    if (const std::uint32_t p = lookup_list(name.c_str())) {
      g_list_names[p] = name;
      found += name + " ";
    }
  };
  for (int i = 0; i < count; ++i) {
    const std::string s = station_name(i);
    if (s.empty()) continue;
    for (const char* pat : kPatterns) add(std::vformat(pat, std::make_format_args(s)));
  }
  for (const char* s : kShared) add(s);
  log::info("Radio probe: {} stations; list names that resolve: {}", count, found);
}

std::string list_name(std::uint32_t p) {
  if (!p) return "-";
  auto it = g_list_names.find(p);
  return it != g_list_names.end() ? it->second : std::format("{:#x}", p);
}

void force(bool next, bool unfreeze) {
  const int idx = invoke<int>(kGetStationIndex);
  Playing p;
  if (idx < 0 || idx >= 32 || !read_station(idx, p) || p.slot > 1) {
    log::info("Radio probe: no station playing (index {})", idx);
    return;
  }
  const std::string st = station_name(idx);
  const auto it = g_list_names.find(p.list);
  if (it == g_list_names.end()) {
    log::info("Radio probe: playing list {:#x} has no known name; can't force", p.list);
    return;
  }
  const std::uint32_t track = next ? p.track + 1 : p.track;
  log::info("Radio probe: FORCE_RADIO_TRACK({}, {}, {}, {}){}", st, it->second, p.category, track,
            unfreeze ? " + unfreeze" : ", staying frozen");
  invoke(kFreeze, st.c_str());
  invoke(kForceTrack, st.c_str(), it->second.c_str(), static_cast<int>(p.category), static_cast<int>(track));
  if (unfreeze) invoke(kUnfreeze, st.c_str());
}

} // namespace

void configure(bool enabled) {
  g_enabled = enabled;
  for (auto& s : g_seen) s = Playing{};
}

void tick() {
  if (!g_enabled) return;
  if (input::pressed_ctrl_shift('1', g_k1)) force(true, true);
  if (input::pressed_ctrl_shift('2', g_k2)) force(true, false);
  if (input::pressed_ctrl_shift('3', g_k3)) force(false, true);

  const DWORD now = GetTickCount();
  if (now - g_last < 1000) return;
  g_last = now;
  const int count = station_count();
  if (count <= 0) return;
  if (!g_names_built) g_names_built = true, build_names(count);

  const int tuned = invoke<int>(kGetStationIndex);
  for (int i = 0; i < count && i < 32; ++i) {
    Playing p;
    if (!read_station(i, p) || p == g_seen[i]) continue;
    g_seen[i] = p;
    log::info("Radio probe: [{}] {}{} slot {} cat {} track {} list {} cur {} frozen {}{}", i, station_name(i),
              i == tuned ? " (tuned)" : "", p.slot, p.category, p.track, list_name(p.list),
              static_cast<int>(p.cur_track), p.frozen, invoke<bool>(kIsRetuning) && i == tuned ? " retuning" : "");
  }
}

} // namespace ivc::radio_probe
