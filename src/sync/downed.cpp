#include "sync/downed.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "core/input.h"
#include "core/log.h"
#include "game/phone.h"
#include "game/player_slots.h"
#include "net/bytes.h"

namespace ivc::downed {
namespace {
namespace n = natives;
using n::invoke;

constexpr std::uint32_t kDownAt = 115, kDownedHealth = 110, kRevivedHealth = 150;
constexpr DWORD kReviveMs = 6000, kBleedOutMs = 60000, kMissionPollMs = 1000;
constexpr float kReviveRange = 1.8f;
constexpr int kReviveKey = 'E';

constexpr std::uint32_t kRequestAnims = 0x65F874DE, kHaveAnimsLoaded = 0x1D3F681D, kTaskPlayAnim = 0x28EE78D8,
                        kGetMissionFlag = 0x2BC64736;
// The engine medics' CPR clips (names from the exe), else the paramedic mission's.
struct Clip { const char* dict; const char* anim; };
constexpr Clip kCprClips[] = {{"medic", "Medic_CPR_LOOP"}, {"Missemergencycall", "Medic_health_inject"}};

constexpr std::uint32_t kRagdoll = 0x1A0F56C5, kIsRagdoll = 0x3E251ADE, kPlayAnimNonInterruptable = 0x52202E76,
                        kSetPlayerControl = 0x1A6203EA;
constexpr const char* kInjuredDict = "missbrian_3";
constexpr const char* kInjuredAnim = "Injured_Idle";

bool g_on_mission = false;     // host: our own flag; joiner: the host's
bool g_sent_flag = false;      // host: last mission flag broadcast
DWORD g_mission_poll = 0;
bool g_downed = false;
DWORD g_downed_since = 0;
bool g_revived = false;        // a teammate finished reviving us
bool g_all_down = false;       // host said everyone is down: die
bool g_invader = false;        // we are an invader (M11): downed all the time, revived only by other invaders
// Reviving someone.
std::uint8_t g_reviving = 0xFF;
DWORD g_revive_since = 0;
const Clip* g_clip = nullptr;
std::string g_prompt;          // help text currently shown (avoid re-showing every frame)

void send(net::Session& net, Msg type, std::uint8_t value) {
  net::Writer w;
  w.put(type).put(std::uint8_t{0}).put(value);
  net.send_game(std::move(w.data()), true);
}

void prompt(const std::string& text) {
  if (text == g_prompt) return;
  g_prompt = text;
  if (text.empty())
    n::ClearHelp();
  else
    phone::notify(text);
}

bool held(int vk) { return input::game_focused() && (GetAsyncKeyState(vk) & 0x8000); }

InjuredPose g_pose; // our own player while downed

// Lethal-damage guard. A player dies below 100 health and goes down at 115, so any hit bigger than the
// gap between (a shotgun blast at 130) killed outright. While on a mission the player's health setters
// (CPlayerPed: set 0xA2D240, set 0xA2B510, add 0xA29E40 - health float at +0xE9C) are kept from going
// below kDownedHealth; a clamped hit puts us down. die() lifts the guard for its own SetCharHealth(0).
constexpr std::uintptr_t kFnSetHealth = 0xA2D240, kFnSetHealth2 = 0xA2B510, kFnAddHealth = 0xA29E40,
                         kPedPool = 0x18B6F1C, kFnPoolAt = 0x499D20, kOffHealth = 0xE9C;
void* g_guard_ped = nullptr;      // our CPed
volatile bool g_guard = false;    // on a mission with downed on
volatile bool g_allow_death = false;
volatile bool g_lethal_hit = false;

template <class T> T at_static(std::uintptr_t va) {
  return reinterpret_cast<T>(va - 0x400000 + reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}

bool guarding(void* ped) { return ped && ped == g_guard_ped && g_guard && !g_allow_death; }

// A partner's copy (one of our player slots) takes engine damage here so it reacts and the hit can be
// reported (Puppet::take_hits_from), but never dies from it: its owner decides. Kept above 100 (dead).
constexpr float kCopyFloor = 101.0f;

float floor_hit(float value) {
  if (value >= static_cast<float>(kDownedHealth)) return value;
  g_lethal_hit = true;
  return static_cast<float>(kDownedHealth);
}

float health_of(void* ped) { return *reinterpret_cast<float*>(static_cast<std::uint8_t*>(ped) + kOffHealth); }

using SetHealthFn = void(__thiscall*)(void*, float, int);
using SetHealth2Fn = void(__thiscall*)(void*, float);
SetHealthFn g_set_health = nullptr;
SetHealth2Fn g_set_health2 = nullptr, g_add_health = nullptr;

void __fastcall set_health_detour(void* ped, void*, float value, int flag) {
  if (player_slots::is_slot_object(ped)) return g_set_health(ped, std::max(value, kCopyFloor), flag);
  g_set_health(ped, guarding(ped) ? floor_hit(value) : value, flag);
}
void __fastcall set_health2_detour(void* ped, void*, float value) {
  if (player_slots::is_slot_object(ped)) return g_set_health2(ped, std::max(value, kCopyFloor));
  g_set_health2(ped, guarding(ped) ? floor_hit(value) : value);
}
void __fastcall add_health_detour(void* ped, void*, float delta) {
  if (player_slots::is_slot_object(ped)) return g_add_health(ped, std::max(health_of(ped) + delta, kCopyFloor) - health_of(ped));
  if (guarding(ped)) {
    const float now = *reinterpret_cast<float*>(static_cast<std::uint8_t*>(ped) + kOffHealth);
    delta = floor_hit(now + delta) - now;
  }
  g_add_health(ped, delta);
}

void* ped_object(n::Ped handle) {
  __try {
    using PoolAt = void*(__thiscall*)(void*, n::Ped);
    return at_static<PoolAt>(kFnPoolAt)(*at_static<void**>(kPedPool), handle);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

void go_down(n::Ped me) {
  g_downed = true;
  g_downed_since = GetTickCount();
  n::SetCharHealth(me, kDownedHealth);
  n::SetCharInvincible(me, true);
  invoke(kSetPlayerControl, n::GetPlayerId(), false); // lying: no moving or shooting
  log::info("Downed: waiting for a partner");
}

void get_up(n::Ped me, std::uint32_t health) {
  g_downed = false;
  g_pose.up(me);
  n::SetCharInvincible(me, false);
  n::SetCharHealth(me, health);
  invoke(kSetPlayerControl, n::GetPlayerId(), true);
  prompt("");
}

void die(n::Ped me) {
  g_downed = false;
  g_pose.up(me);
  invoke(kSetPlayerControl, n::GetPlayerId(), true);
  n::SetCharInvincible(me, false);
  g_allow_death = true;
  n::SetCharHealth(me, 0);
  g_allow_death = false;
  prompt("");
}

const Clip* cpr_clip() {
  for (const Clip& c : kCprClips) {
    invoke(kRequestAnims, c.dict);
    if (invoke<bool>(kHaveAnimsLoaded, c.dict)) return &c;
  }
  return nullptr;
}

void stop_reviving(n::Ped me) {
  if (g_reviving != 0xFF) n::ClearCharTasks(me);
  g_reviving = 0xFF;
}

} // namespace

void InjuredPose::update(n::Ped ped, std::uint32_t now) {
  if (!ped || !n::DoesCharExist(ped)) return;
  switch (state_) {
    case State::Standing:
      n::ClearCharTasks(ped);
      invoke(kRagdoll, ped, 0, 2500, 1, 1, 1, 0); // fall where they stand
      invoke(kRequestAnims, kInjuredDict);
      state_ = State::Falling, since_ = now;
      break;
    case State::Falling:
      invoke(kRequestAnims, kInjuredDict);
      if (now - since_ < 1500 || (invoke<bool>(kIsRagdoll, ped) && now - since_ < 6000)) break;
      if (!invoke<bool>(kHaveAnimsLoaded, kInjuredDict)) break;
      invoke(kPlayAnimNonInterruptable, ped, kInjuredAnim, kInjuredDict, 8.0f, 1, 0, 0, 0, -2);
      state_ = State::Lying, since_ = now;
      break;
    case State::Lying:
      // Re-assert now and then (an impact can knock the ped out of the loop).
      if (now - since_ > 4000) {
        since_ = now;
        if (!invoke<bool>(kIsRagdoll, ped))
          invoke(kPlayAnimNonInterruptable, ped, kInjuredAnim, kInjuredDict, 8.0f, 1, 0, 0, 0, -2);
      }
      break;
  }
}

void InjuredPose::up(n::Ped ped) {
  if (state_ == State::Standing) return;
  state_ = State::Standing;
  if (ped && n::DoesCharExist(ped)) n::ClearCharTasks(ped);
}

bool install() {
  struct Hook { std::uintptr_t va; void* detour; void** orig; };
  const Hook hooks[] = {{kFnSetHealth, reinterpret_cast<void*>(&set_health_detour), reinterpret_cast<void**>(&g_set_health)},
                        {kFnSetHealth2, reinterpret_cast<void*>(&set_health2_detour), reinterpret_cast<void**>(&g_set_health2)},
                        {kFnAddHealth, reinterpret_cast<void*>(&add_health_detour), reinterpret_cast<void**>(&g_add_health)}};
  for (const Hook& h : hooks) {
    void* target = at_static<void*>(h.va);
    if (MH_CreateHook(target, h.detour, h.orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
      log::error("Downed: could not hook the player health setter at {:#x}", h.va);
      return false;
    }
  }
  log::info("Downed: lethal-damage guard installed");
  return true;
}

bool is_msg(std::span<const std::uint8_t> d) {
  return !d.empty() && d[0] >= static_cast<std::uint8_t>(Msg::MissionFlag) &&
         d[0] <= static_cast<std::uint8_t>(Msg::AllDown);
}

void on_message(const net::GameMessage& m, std::uint8_t my_id) {
  net::Reader r(m.data);
  Msg type;
  std::uint8_t sender, value = 0;
  if (!r.get(type) || !r.get(sender) || !r.get(value)) return;
  switch (type) {
    case Msg::MissionFlag:
      if (m.from == net::kHostId) g_on_mission = value != 0;
      break;
    case Msg::Revive:
      if (value == my_id && g_downed) g_revived = true;
      break;
    case Msg::AllDown: // the co-op group's: invaders go down on their own terms (sync/invasion)
      if (m.from == net::kHostId && !g_invader) g_all_down = true;
      break;
  }
}

void tick(const Frame& f, net::Session& net) {
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me || !n::DoesCharExist(me)) return;
  const DWORD now = GetTickCount();
  g_guard_ped = ped_object(me);
  g_invader = f.in_session && f.invader;
  g_guard = f.in_session && (g_on_mission || f.invader);
  if (!f.in_session) {
    g_guard = false;
    if (g_downed) get_up(me, kRevivedHealth);
    clear();
    return;
  }
  if (f.hosting && now - g_mission_poll > kMissionPollMs) { // the host decides what "on a mission" is
    g_mission_poll = now;
    g_on_mission = invoke<bool>(kGetMissionFlag);
    if (g_on_mission != g_sent_flag) {
      g_sent_flag = g_on_mission;
      send(net, Msg::MissionFlag, g_on_mission);
      log::info("Downed: mission {}", g_on_mission ? "started (downed/revive on)" : "over");
    }
  }

  if (g_all_down) {
    g_all_down = false;
    stop_reviving(me);
    if (g_downed) die(me);
    return;
  }

  // --- our own state
  if (g_downed) {
    if (g_revived) {
      g_revived = false;
      get_up(me, kRevivedHealth);
      log::info("Downed: revived");
    } else if (!g_on_mission && !f.invader) {
      get_up(me, kRevivedHealth); // the mission ended while we were down (invaders stay down until revived)
    } else if (!f.hosting && !f.invader && now - g_downed_since > kBleedOutMs) { // invaders: the host ends the invasion when all are down
      log::info("Downed: bled out");
      die(me);
    } else {
      n::SetCharHealth(me, kDownedHealth);
      g_pose.update(me, now);
      const DWORD left = f.hosting ? 0 : (kBleedOutMs - (now - g_downed_since)) / 1000;
      prompt(f.invader ? "You're down! Another invader can revive you." : f.hosting ? "You're down! A partner can revive you." : std::format("You're down! A partner can revive you ({} s).", left / 10 * 10 + 10));
    }
  } else if ((g_on_mission || f.invader) && !n::IsCharDead(me) && (g_lethal_hit || n::GetCharHealth(me) <= kDownAt)) {
    if (g_lethal_hit) log::info("Downed: a lethal hit was held at {}", kDownedHealth);
    go_down(me);
  }
  g_lethal_hit = false;

  // --- host: everyone down at once -> everyone dies (the mission fails as normal)
  if (f.hosting && g_on_mission && g_downed && !f.teammates.empty()) {
    bool all = true;
    for (const Teammate& t : f.teammates) all = all && t.downed;
    if (all) {
      log::info("Downed: everyone is down");
      send(net, Msg::AllDown, 0);
      die(me);
      return;
    }
  }

  // --- reviving a teammate
  if (g_downed || n::IsCharDead(me)) {
    stop_reviving(me);
    return;
  }
  const Teammate* target = nullptr;
  const n::Vector3 at = n::GetCharCoordinates(me);
  for (const Teammate& t : f.teammates) {
    if (!t.downed || !t.ped || !n::DoesCharExist(t.ped)) continue;
    const n::Vector3 p = n::GetCharCoordinates(t.ped);
    if (std::hypot(p.x - at.x, p.y - at.y) < kReviveRange && std::fabs(p.z - at.z) < 2.0f) target = &t;
  }
  if (!target) {
    stop_reviving(me);
    if (g_prompt.rfind("Hold E", 0) == 0 || g_prompt.rfind("Reviving", 0) == 0) prompt("");
    return;
  }
  if (!held(kReviveKey)) {
    stop_reviving(me);
    prompt("Hold E to revive " + target->name + ".");
    return;
  }
  if (g_reviving != target->id) {
    g_reviving = target->id;
    g_revive_since = now;
    g_clip = cpr_clip();
    if (g_clip) invoke(kTaskPlayAnim, me, g_clip->anim, g_clip->dict, 8.0f, 1, 0, 0, 0, -1);
    prompt("Reviving " + target->name + "...");
  }
  if (now - g_revive_since >= kReviveMs) {
    send(net, Msg::Revive, target->id);
    log::info("Downed: revived {} (#{})", target->name, target->id);
    stop_reviving(me);
    prompt(target->name + " is back up.");
  }
}

bool local_downed() { return g_downed; }

std::uint32_t floor_health(std::uint32_t health) {
  return (g_on_mission || g_invader) && health < kDownedHealth ? kDownedHealth : health;
}

void clear() {
  g_on_mission = g_sent_flag = false;
  g_revived = g_all_down = false;
  g_reviving = 0xFF;
  if (!g_downed && !g_prompt.empty()) prompt("");
}

} // namespace ivc::downed
