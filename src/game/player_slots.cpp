#include "game/player_slots.h"

#include <windows.h>

#include <MinHook.h>

#include <array>

#include "core/log.h"

namespace ivc::player_slots {
namespace {
namespace n = natives;
using n::invoke;

// Engine internals (static VAs, GTAIV.exe 1.2.0.59, image base 0x400000).
//   create(flags*, slot, model_index, flag) -> CPed*: allocates the slot's CPlayerInfo and
//     a player ped. The flags are copied into the ped (+0x218/+0x219). CREATE_PLAYER passes
//     {0,1}, which makes the slot the local one (camera, pad). {1,0} crashed: with +0x219
//     clear the ctor runs an init step needing +0x22c, which the factory sets afterwards.
//     {1,1} skips both: a remote-style player ped.
//   set_pop_type(CPed* this, type): CREATE_PLAYER passes 2 right after creating.
//   place(CPed*, pos*): sets position, adds the ped to the world (0x9495D0), finishes setup.
//     CREATE_PLAYER then gives the ped its default player-controlled task; we don't.
//   model_index(name)
constexpr std::uintptr_t kFnCreatePlayerSlot = 0x0093E960;
constexpr std::uintptr_t kFnSetPopType = 0x009F19C0;
constexpr std::uintptr_t kFnPlacePlayerPed = 0x0093E380;
constexpr std::uintptr_t kFnModelIndex = 0x009DC680;
constexpr std::uintptr_t kPlayerInfos = 0x011A8808; // CPlayerInfo* [32]
// get_pad(CPed* this): a player ped's controller; null for remote (+0x218) player peds.
// The per-frame player-control code uses it unchecked (online, remote players are network
// clones and never run it), so our peds crashed a frame after spawning. The local pad
// made them copy our moves (SET_PLAYER_CONTROL off on their slot doesn't stop the idle
// on-foot task reading it), so they get the second pad: the one the local-pad getter
// (0x8F69E0) falls back to, which nothing drives in single player.
constexpr std::uintptr_t kFnGetPad = 0x00A2B350;
constexpr std::uintptr_t kIdlePad = 0x01185C08;

constexpr std::uint32_t kSetPlayerControl = 0x1A6203EA;
constexpr std::uint32_t kSetPoliceIgnorePlayer = 0x619D51D3;
constexpr std::uint32_t kSetEveryoneIgnorePlayer = 0x059901B9;
constexpr std::uint32_t kConvertIntToPlayerIndex = 0x5996315E;
constexpr std::uint32_t kPlayerHasChar = 0x22545844;

constexpr std::uint32_t kFirstSlot = 1, kSlots = 3; // slots 1..3; 0 is the local player

template <class T> T at_static(std::uintptr_t va) {
  return reinterpret_cast<T>(va - 0x400000 + reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}

void* slot_info(std::uint32_t slot) { return at_static<void**>(kPlayerInfos)[slot]; }

struct Slot {
  void* obj = nullptr; // CPed*
  n::Ped ped = 0;      // script handle
  bool in_use = false;
  std::uint32_t model = kProtagonistModel;
};

constexpr std::uintptr_t kPedPool = 0x18B6F1C, kFnPoolAt = 0x499D20; // script handle -> CPed*
constexpr std::uint32_t kChangePlayerModel = 0x232F1A85;
std::array<Slot, kSlots> g_slots{};

using GetPadFn = void*(__thiscall*)(void*);
GetPadFn g_get_pad_orig = nullptr;

void* __fastcall get_pad_detour(void* ped, void* /*edx*/) {
  if (ped)
    for (const Slot& s : g_slots)
      if (s.obj == ped) return at_static<void*>(kIdlePad);
  return g_get_pad_orig(ped);
}

bool hook_get_pad() {
  if (g_get_pad_orig) return true;
  void* target = at_static<void*>(kFnGetPad);
  if (MH_CreateHook(target, reinterpret_cast<void*>(&get_pad_detour), reinterpret_cast<void**>(&g_get_pad_orig)) !=
          MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    log::error("PlayerSlots: could not hook the pad getter");
    return false;
  }
  return true;
}

void* guarded_create(std::uint32_t slot, std::int32_t model) {
  using Create = void*(__cdecl*)(const std::uint8_t*, std::uint32_t, std::int32_t, std::uint8_t);
  const std::uint8_t flags[2] = {1, 1};
  __try {
    return at_static<Create>(kFnCreatePlayerSlot)(flags, slot, model, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return reinterpret_cast<void*>(-1);
  }
}

bool guarded_place(void* ped, n::Vector3 at) {
  __try {
    at_static<void(__thiscall*)(void*, std::uint32_t)>(kFnSetPopType)(ped, 2);
    at_static<bool(__cdecl*)(void*, n::Vector3*)>(kFnPlacePlayerPed)(ped, &at);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

n::Ped handle_of(std::uint32_t slot) {
  const n::Player p = invoke<n::Player>(kConvertIntToPlayerIndex, slot);
  return invoke<bool>(kPlayerHasChar, p) ? n::GetPlayerChar(p) : 0;
}

// A slot we created whose ped is still there (a save load may wipe player slots).
bool alive(std::uint32_t i) {
  const Slot& s = g_slots[i];
  const std::uint32_t slot = kFirstSlot + i;
  return s.obj && slot_info(slot) && handle_of(slot) == s.ped && n::DoesCharExist(s.ped);
}

void* object_of(n::Ped handle) {
  __try {
    return at_static<void*(__thiscall*)(void*, n::Ped)>(kFnPoolAt)(*at_static<void**>(kPedPool), handle);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// Puts `model` on slot i's player (CHANGE_PLAYER_MODEL recreates the ped: new handle and CPed*). The
// model must be loaded.
bool wear(std::uint32_t i, std::uint32_t model) {
  Slot& s = g_slots[i];
  if (s.model == model) return true;
  const std::uint32_t slot = kFirstSlot + i;
  invoke(kChangePlayerModel, invoke<n::Player>(kConvertIntToPlayerIndex, slot), model);
  const n::Ped ped = handle_of(slot);
  void* obj = ped ? object_of(ped) : nullptr;
  if (!ped || !obj) {
    log::error("PlayerSlots: changing slot {}'s model to 0x{:08X} lost its ped", slot, model);
    return false;
  }
  s.ped = ped, s.obj = obj, s.model = model;
  log::info("PlayerSlots: slot {} now wears 0x{:08X} (ped {})", slot, model, ped);
  return true;
}

void park(n::Ped ped) {
  n::ClearCharTasks(ped);
  n::SetCharVisible(ped, false);
  n::SetCharCollision(ped, false);
  n::SetCharCoordinatesNoOffset(ped, {0.0f, 0.0f, -100.0f});
  n::FreezeCharPosition(ped, true);
}

void place(n::Ped ped, const n::Vector3& at, float heading) {
  n::FreezeCharPosition(ped, false);
  n::SetCharCollision(ped, true);
  n::SetCharCoordinatesNoOffset(ped, at);
  n::SetCharHeading(ped, heading);
  n::SetCharVisible(ped, true);
}

} // namespace

n::Ped acquire(const n::Vector3& at, float heading, std::uint32_t model) {
  if (!hook_get_pad()) return 0;
  // Reuse a parked slot of ours first.
  for (std::uint32_t i = 0; i < kSlots; ++i) {
    Slot& s = g_slots[i];
    if (s.in_use || !s.obj) continue;
    if (!alive(i)) {
      log::info("PlayerSlots: slot {} is gone (save load?); recreating it", kFirstSlot + i);
      s = {};
      continue;
    }
    if (!wear(i, model)) return 0;
    place(s.ped, at, heading);
    s.in_use = true;
    log::info("PlayerSlots: reusing slot {} (ped {})", kFirstSlot + i, s.ped);
    return s.ped;
  }
  // Otherwise create one in a free slot.
  for (std::uint32_t i = 0; i < kSlots; ++i) {
    Slot& s = g_slots[i];
    const std::uint32_t slot = kFirstSlot + i;
    if (s.obj || slot_info(slot)) continue; // ours (in use) or the engine's
    const std::int32_t model = at_static<std::int32_t(__cdecl*)(const char*)>(kFnModelIndex)("player");
    void* obj = guarded_create(slot, model);
    if (obj == reinterpret_cast<void*>(-1) || !obj) {
      log::error("PlayerSlots: creating slot {} failed ({})", slot, obj ? "engine fault" : "no ped");
      // Drop a half-built slot (leaking it) so the per-frame player update skips it.
      at_static<void**>(kPlayerInfos)[slot] = nullptr;
      return 0;
    }
    s.obj = obj; // before it is in the world, so its first update gets a pad
    if (!guarded_place(obj, at)) {
      log::error("PlayerSlots: placing slot {} faulted", slot);
      return 0;
    }
    s.ped = handle_of(slot);
    if (!s.ped) {
      log::error("PlayerSlots: slot {} has no script handle", slot);
      return 0;
    }
    const n::Player p = invoke<n::Player>(kConvertIntToPlayerIndex, slot);
    invoke(kSetPlayerControl, p, false);
    invoke(kSetPoliceIgnorePlayer, p, true);
    invoke(kSetEveryoneIgnorePlayer, p, true);
    if (!wear(i, model)) return 0;
    place(s.ped, at, heading);
    s.in_use = true;
    log::info("PlayerSlots: created slot {} (ped {})", slot, s.ped);
    return s.ped;
  }
  log::warn("PlayerSlots: all {} extra player slots are in use", kSlots);
  return 0;
}

void release(n::Ped ped) {
  for (std::uint32_t i = 0; i < kSlots; ++i) {
    Slot& s = g_slots[i];
    if (!s.in_use || s.ped != ped) continue;
    s.in_use = false;
    if (n::DoesCharExist(ped)) park(ped);
    log::info("PlayerSlots: parked slot {}", kFirstSlot + i);
    return;
  }
}

bool owns(n::Ped ped) {
  for (const Slot& s : g_slots)
    if (s.in_use && s.ped == ped) return true;
  return false;
}

} // namespace ivc::player_slots
