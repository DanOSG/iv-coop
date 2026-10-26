#include "game/radio.h"

#include <windows.h>

#include "game/natives.h"

namespace ivc::radio {
namespace {
namespace n = natives;
using n::invoke;

// Static VAs, GTAIV.exe 1.2.0.59 (image base 0x400000).
constexpr std::uintptr_t kStationArray = 0x011D76AC; // station* []
constexpr std::uintptr_t kStationCount = 0x011D74F1; // byte
constexpr std::uintptr_t kFnResolveItem = 0x00945230; // thiscall(station, category, 0, track) -> item
constexpr std::uintptr_t kFnSlotStop = 0x00B76C10;    // thiscall(slot)
constexpr std::uintptr_t kFnSlotLoad = 0x00B76660;    // thiscall(slot, category, track, item, 0)
constexpr std::uintptr_t kFnSlotStart = 0x00B769A0;   // thiscall(slot, meta, start_ms, flag_a, flag_b)
constexpr std::uintptr_t kFnSlotPlay = 0x00B76920;    // thiscall(slot)

// Station: two slots of kSlotSize, current slot index byte, per-slot metadata, current/previous track.
constexpr std::uint32_t kSlotSize = 0xBD0, kOffCurSlot = 0x1917, kOffSlotMeta = 0x17A0, kOffPrevTrack = 0x1908,
                        kOffCurTrack = 0x190C, kOffStartFlagA = 0x191A, kOffStartFlagB = 0x191B;
// Slot: category, track, position (ms), the sound, state (1 idle, 2 starting, 3 playing, 4 stopping), active.
constexpr std::uint32_t kSlotCategory = 0xBC0, kSlotTrack = 0xBC4, kSlotPosition = 0x990, kSlotSound = 0x964,
                        kSlotState = 0xBC8, kSlotActive = 0xBCA;
constexpr std::uint8_t kIdle = 1, kStarting = 2, kPlaying = 3;

constexpr std::uint32_t kGetStationIndex = 0x4E493AAF, kRetuneToIndex = 0x48ED6432;

template <class T> T at_static(std::uintptr_t va) {
  return reinterpret_cast<T>(va - 0x400000 + reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}

std::uint8_t* station_ptr(int station) {
  if (station < 0 || station >= *at_static<std::uint8_t*>(kStationCount)) return nullptr;
  return (*at_static<std::uint8_t***>(kStationArray))[station];
}

std::uint8_t* current_slot(std::uint8_t* st) {
  return st && st[kOffCurSlot] <= 1 ? st + st[kOffCurSlot] * kSlotSize : nullptr;
}

template <class T> T& field(std::uint8_t* p, std::uint32_t off) { return *reinterpret_cast<T*>(p + off); }

} // namespace

int station_count() {
  __try {
    return *at_static<std::uint8_t*>(kStationCount);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

StationState read(int station) {
  StationState s;
  __try {
    std::uint8_t* slot = current_slot(station_ptr(station));
    if (!slot) return s;
    s.item.category = slot[kSlotCategory];
    s.item.track = static_cast<std::uint16_t>(field<std::uint32_t>(slot, kSlotTrack));
    s.position = field<std::uint32_t>(slot, kSlotPosition);
    s.audible = (slot[kSlotState] == kStarting || slot[kSlotState] == kPlaying) && field<void*>(slot, kSlotSound);
    s.valid = slot[kSlotActive] != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    s.valid = false;
  }
  return s;
}

bool play(int station, Item item, std::uint32_t offset_ms, bool audible) {
  __try {
    std::uint8_t* st = station_ptr(station);
    std::uint8_t* slot = current_slot(st);
    if (!slot) return false;
    using Resolve = std::uint32_t(__thiscall*)(void*, int, int, int);
    const std::uint32_t entry = at_static<Resolve>(kFnResolveItem)(st, item.category, 0, item.track);
    if (!entry) return false;
    if (slot[kSlotActive]) at_static<void(__thiscall*)(void*)>(kFnSlotStop)(slot);
    at_static<void(__thiscall*)(void*, int, int, std::uint32_t, int)>(kFnSlotLoad)(slot, item.category, item.track,
                                                                                    entry, 0);
    field<std::uint32_t>(slot, kSlotPosition) = offset_ms;
    if (audible) {
      at_static<void(__thiscall*)(void*, std::uint32_t, std::uint32_t, int, int)>(kFnSlotStart)(
          slot, field<std::uint32_t>(st, kOffSlotMeta + 4 * st[kOffCurSlot]), offset_ms, st[kOffStartFlagA],
          st[kOffStartFlagB]);
      at_static<void(__thiscall*)(void*)>(kFnSlotPlay)(slot);
    } else {
      slot[kSlotState] = kIdle; // positioned; streams from offset_ms when tuned
    }
    field<std::uint32_t>(st, kOffPrevTrack) = field<std::uint32_t>(st, kOffCurTrack);
    field<std::uint32_t>(st, kOffCurTrack) = item.track;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void set_idle_position(int station, std::uint32_t offset_ms) {
  __try {
    std::uint8_t* slot = current_slot(station_ptr(station));
    if (slot && slot[kSlotState] == kIdle) field<std::uint32_t>(slot, kSlotPosition) = offset_ms;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

int player_station() { return invoke<int>(kGetStationIndex); }

void retune(int player_station) { invoke(kRetuneToIndex, player_station); }

} // namespace ivc::radio
