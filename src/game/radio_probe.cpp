#include "game/radio_probe.h"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstring>
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
constexpr std::uint32_t kSlotPosition = 0x990; // play position, ms (rises 1000/s while playing)
constexpr std::uint32_t kSlotSeek = 0x99C;  // int32 correction added to the position (0x946E70)
constexpr std::uint32_t kSlotActive = 0xBCA; // set to 1 when the slot starts; ForceTrack stops it first if set

constexpr std::uint32_t kGetStationIndex = 0x4E493AAF, kGetRadioName = 0x7EC9580E, kIsRetuning = 0x45C344AA;

template <class T> T at_static(std::uintptr_t va) {
  return reinterpret_cast<T>(va - 0x400000 + reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}

// Both stream slots: the station crossfades between them, and DJ talk can overlap a song's intro.
struct SlotState {
  std::uint8_t category = 0, active = 0;
  std::uint32_t track = ~0u, list = 0;
  bool operator==(const SlotState&) const = default;
};
struct Playing {
  std::uint8_t slot = 0xFF, frozen = 0;
  std::uint32_t cur_track = ~0u;
  std::array<SlotState, 2> slots{};
  // The current slot's values (what ForceTrack takes).
  std::uint8_t category = 0;
  std::uint32_t track = ~0u, list = 0;
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
    for (int k = 0; k < 2; ++k) {
      const std::uint8_t* slot = st + k * kSlotSize;
      SlotState& ss = out.slots[k];
      ss.category = slot[kSlotCategory];
      ss.active = slot[kSlotActive];
      ss.track = *reinterpret_cast<const std::uint32_t*>(slot + kSlotTrack);
      ss.list = *reinterpret_cast<const std::uint32_t*>(slot + kSlotList);
    }
    if (out.slot > 1) return true;
    out.category = out.slots[out.slot].category;
    out.track = out.slots[out.slot].track;
    out.list = out.slots[out.slot].list;
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

// Play-position hunt: dwords in the tuned station's playing slot that rise every second by about a second's
// worth (ms, or samples at 22.05/32/44.1/48 kHz). Hits are counted per offset and logged once they repeat.
std::array<std::uint32_t, kSlotSize / 4> g_prev_slot{};
int g_prev_station = -1, g_prev_slot_index = -1;
std::unordered_map<std::uint32_t, int> g_rising; // offset -> consecutive matching seconds
std::unordered_map<std::uint32_t, bool> g_reported;

bool copy_slot(int station, int slot, std::array<std::uint32_t, kSlotSize / 4>& out) {
  __try {
    auto* st = (*at_static<std::uint8_t***>(kStationArray))[station];
    if (!st) return false;
    memcpy(out.data(), st + slot * kSlotSize, kSlotSize);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void hunt_position(int station, int slot, DWORD elapsed_ms) {
  std::array<std::uint32_t, kSlotSize / 4> cur{};
  if (station < 0 || slot < 0 || slot > 1 || !copy_slot(station, slot, cur)) return;
  if (station == g_prev_station && slot == g_prev_slot_index && elapsed_ms >= 800 && elapsed_ms <= 1500) {
    const double sec = elapsed_ms / 1000.0;
    for (std::uint32_t i = 0; i < cur.size(); ++i) {
      const std::int64_t d = static_cast<std::int64_t>(cur[i]) - g_prev_slot[i];
      bool match = false;
      for (double rate : {1000.0, 22050.0, 32000.0, 44100.0, 48000.0})
        if (d > 0 && std::abs(d - rate * sec) < rate * sec * 0.15) match = true;
      int& n = g_rising[i * 4];
      n = match ? n + 1 : 0;
      if (n == 5 && !g_reported[i * 4]) {
        g_reported[i * 4] = true;
        log::info("Radio probe: slot +{:#x} rises steadily ({} now, +{} in {} ms): play-position candidate", i * 4,
                  cur[i], d, elapsed_ms);
      }
    }
  } else {
    g_rising.clear();
  }
  g_prev_slot = cur, g_prev_station = station, g_prev_slot_index = slot;
}

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
  if (it != g_list_names.end()) return it->second;
  static const std::uint32_t anchor = lookup_list("RADIO_ADVERTS");
  return std::format("ADVERTS{:+#x}", static_cast<std::int64_t>(p) - anchor);
}

// Seeks the playing item: slot +0x99C is a correction added to the stream position, the way the engine's own
// network radio sync (0x946E70) moves a playing slot to a remote position: target - current position.
bool seek(int station, std::uint32_t target_ms) {
  __try {
    auto* st = (*at_static<std::uint8_t***>(kStationArray))[station];
    if (!st || st[kOffCurSlot] > 1) return false;
    std::uint8_t* slot = st + st[kOffCurSlot] * kSlotSize;
    *reinterpret_cast<std::int32_t*>(slot + kSlotSeek) =
        static_cast<std::int32_t>(target_ms - *reinterpret_cast<std::uint32_t*>(slot + kSlotPosition));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Starts item `item` (a track entry pointer: entries of one list are 8 bytes apart, so track n is
// list + 8n) on the station's current slot, as ForceTrack 0x945120 does but without its name lookup and
// frozen check: stop the slot (0xB76C10) if busy, load (0xB76660), start (0xB769A0, 0xB76920), then record
// it as the station's current track.
bool start_item(int station, std::uint32_t item, std::uint8_t category, std::uint32_t track) {
  __try {
    auto* st = (*at_static<std::uint8_t***>(kStationArray))[station];
    if (!st || !item) return false;
    const std::uint8_t cur = st[kOffCurSlot];
    if (cur > 1) return false;
    std::uint8_t* slot = st + cur * kSlotSize;
    if (slot[kSlotActive]) at_static<void(__thiscall*)(void*)>(0xB76C10)(slot);
    at_static<void(__thiscall*)(void*, int, int, std::uint32_t, int)>(0xB76660)(slot, category, static_cast<int>(track),
                                                                                 item, 0);
    const std::uint32_t meta = *reinterpret_cast<std::uint32_t*>(st + 0x17A0 + 4 * cur);
    at_static<void(__thiscall*)(void*, std::uint32_t, int, int, int)>(0xB769A0)(slot, meta, 0, st[0x191A], st[0x191B]);
    at_static<void(__thiscall*)(void*)>(0xB76920)(slot);
    *reinterpret_cast<std::uint32_t*>(st + 0x1908) = *reinterpret_cast<std::uint32_t*>(st + kOffCurTrack);
    *reinterpret_cast<std::uint32_t*>(st + kOffCurTrack) = *reinterpret_cast<std::uint32_t*>(slot + kSlotTrack);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

std::uint32_t position(int station, int slot) {
  __try {
    auto* st = (*at_static<std::uint8_t***>(kStationArray))[station];
    return st && slot <= 1 ? *reinterpret_cast<std::uint32_t*>(st + slot * kSlotSize + kSlotPosition) : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// Ctrl+Shift+1: the next track of the playing list, from its start. 2: the playing item, 30 s further on.
// 3: the playing item from its start.
void force(int key) {
  const int idx = invoke<int>(kGetStationIndex);
  Playing p;
  if (idx < 0 || idx >= 32 || !read_station(idx, p) || p.slot > 1 || !p.list) {
    log::info("Radio probe: no station playing (index {})", idx);
    return;
  }
  const std::uint32_t pos = position(idx, p.slot);
  bool ok = true;
  if (key == 1) ok = start_item(idx, p.list + 8, p.category, p.track + 1);
  ok = ok && seek(idx, key == 2 ? pos + 30000 : 0);
  log::info("Radio probe: key {}: [{}] {} cat {} #{} at {} ms -> {}: {}", key, idx, list_name(p.list), p.category, p.track,
            pos, key == 1 ? "next item from 0" : key == 2 ? "+30 s" : "from 0", ok ? "ok" : "FAILED");
}

} // namespace

void configure(bool enabled) {
  g_enabled = enabled;
  for (auto& s : g_seen) s = Playing{};
}

void tick() {
  if (!g_enabled) return;
  if (input::pressed_ctrl_shift('1', g_k1)) force(1);
  if (input::pressed_ctrl_shift('2', g_k2)) force(2);
  if (input::pressed_ctrl_shift('3', g_k3)) force(3);

  const DWORD now = GetTickCount();
  if (now - g_last < 1000) return;
  g_last = now;
  const int count = station_count();
  if (count <= 0) return;
  if (!g_names_built) g_names_built = true, build_names(count);

  static DWORD prev_hunt = 0;
  const int tuned = invoke<int>(kGetStationIndex);
  if (Playing t; tuned >= 0 && tuned < 32 && read_station(tuned, t)) hunt_position(tuned, t.slot, now - prev_hunt);
  prev_hunt = now;
  for (int i = 0; i < count && i < 32; ++i) {
    Playing p;
    if (!read_station(i, p) || p == g_seen[i]) continue;
    g_seen[i] = p;
    std::string slots;
    for (int k = 0; k < 2; ++k) {
      const SlotState& ss = p.slots[k];
      slots += std::format(" | {}{}: {} cat {} #{} {}", k, k == p.slot ? "*" : "", ss.active ? "on" : "off", ss.category,
                           static_cast<int>(ss.track), list_name(ss.list));
    }
    log::info("Radio probe: [{}] {}{} cur {} frozen {} pos {}{}{}", i, station_name(i), i == tuned ? " (tuned)" : "",
              static_cast<int>(p.cur_track), p.frozen, p.slot <= 1 ? position(i, p.slot) : 0,
              invoke<bool>(kIsRetuning) && i == tuned ? " retuning" : "", slots);
  }
}

} // namespace ivc::radio_probe
