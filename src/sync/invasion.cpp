#include "sync/invasion.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <random>
#include <utility>

#include "core/log.h"
#include "game/phone.h"
#include "net/bytes.h"
#include "render/banner.h"
#include "sync/session_settings.h"

namespace ivc::invasion {
namespace {
namespace n = natives;
using n::invoke;

constexpr std::uint32_t kGetMissionFlag = 0x2BC64736, kSetPoliceIgnorePlayer = 0x619D51D3, kLoadScene = 0x39F62BFB,
                        kRequestCollision = 0x12ED0BC9, kGetCharWeaponInSlot = 0x74EC7580, kSetCarHeading = 0x75E40528;

// eWeaponType ids (as in sync/team_fire): the FBI loadout.
constexpr std::uint32_t kWeaponPistol = 7, kWeaponShotgun = 10, kWeaponCarbine = 15;

constexpr DWORD kPollMs = 500;        // host: re-check the window
constexpr DWORD kCloseDelayMs = 5000; // host: the window stays open this long after the reason ends
constexpr DWORD kKickDelayMs = 1500;  // host: invaders get the "closed" message before being disconnected
constexpr DWORD kResendMs = 3000;     // host: the Open flag, for late joiners
constexpr std::uint32_t kWantedToOpen = 3;
constexpr float kMinSpawnM = 200.0f, kMaxSpawnM = 400.0f;
constexpr int kWeaponSlots = 12;

void send(net::Session& net, Sub sub, std::uint8_t value) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(static_cast<std::uint8_t>(sub)).put(value);
  net.send_game(std::move(w.data()), true);
}

float dist2d(const n::Vector3& a, const n::Vector3& b) { return std::hypot(a.x - b.x, a.y - b.y); }

// --- host: the window ---
bool g_open = false;
DWORD g_poll = 0, g_closing_since = 0, g_last_send = 0, g_kick_at = 0;
bool g_result_sent = false, g_down_sent = false;
int g_invaders_seen = 0;

void host_close(net::Session& net, bool had_invaders) {
  g_open = false;
  g_closing_since = 0;
  net.set_invasion_open(false);
  if (had_invaders) {
    send(net, Sub::Result, static_cast<std::uint8_t>(Result::WindowClosed));
    g_kick_at = GetTickCount() + kKickDelayMs;
  }
  send(net, Sub::Open, 0);
  log::info("Invasion: the window closed");
}

void host_tick(const Frame& f, net::Session& net) {
  const DWORD now = GetTickCount();
  int invaders = 0;
  for (const Player& p : f.players) invaders += p.role == net::Role::Invader;
  if (invaders > g_invaders_seen) phone::notify("Invaders have entered your game: FBI agents are hunting you!");
  if (invaders > 0 && g_invaders_seen == 0) { // the first one in: the invaded group gets the banner
    banner::show(banner::Kind::Invading, "FBI INVADING");
    send(net, Sub::Arrived, 1);
  }
  g_invaders_seen = invaders;
  if (!invaders) g_down_sent = false;

  if (g_kick_at && now >= g_kick_at) {
    g_kick_at = 0;
    net.kick_invaders(net::Reject::NotOpen);
  }

  const bool allowed = session_settings::current().invasions;
  if (now - g_poll >= kPollMs) {
    g_poll = now;
    const bool reason = allowed && (invoke<bool>(kGetMissionFlag) || n::StoreWantedLevel(n::GetPlayerId()) >= kWantedToOpen);
    if (reason) {
      g_closing_since = 0;
      if (!g_open) {
        g_open = true;
        net.set_invasion_open(true);
        send(net, Sub::Open, 1);
        g_last_send = now;
        log::info("Invasion: the window opened (mission or {}+ stars)", kWantedToOpen);
      }
    } else if (g_open) {
      if (!allowed) g_closing_since = now - kCloseDelayMs; // the setting was turned off: no grace
      if (!g_closing_since) g_closing_since = now;
      if (now - g_closing_since >= kCloseDelayMs) host_close(net, invaders > 0);
    }
  }
  if (g_open && now - g_last_send >= kResendMs) {
    g_last_send = now;
    send(net, Sub::Open, 1);
  }

  // Every invader down or dead at once (nobody left to revive them): the invasion failed.
  if (invaders > 0 && !g_down_sent) {
    bool all = true;
    for (const Player& p : f.players)
      if (p.role == net::Role::Invader) all = all && p.known_pos && (p.downed || p.dead);
    if (all) {
      g_down_sent = true;
      send(net, Sub::Result, static_cast<std::uint8_t>(Result::InvadersDown));
      banner::show(banner::Kind::Victory, "FBI VANQUISHED");
      g_kick_at = now + 2500; // they leave on their own; this is the backstop
      log::info("Invasion: every invader is down; the invasion failed");
    }
  }

  // The target dying is the invaders' win.
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  const bool dead = me && n::DoesCharExist(me) && n::IsCharDead(me);
  if (dead && invaders > 0 && !g_result_sent) {
    g_result_sent = true;
    send(net, Sub::Result, static_cast<std::uint8_t>(Result::TargetDied));
    banner::show(banner::Kind::Defeat, "DEFEATED BY THE FBI");
    log::info("Invasion: the target died; the invaders win");
  }
  if (!dead) g_result_sent = false;
}

void host_reset(net::Session& net) {
  if (g_open) net.set_invasion_open(false);
  g_open = false, g_closing_since = 0, g_kick_at = 0, g_result_sent = g_down_sent = false, g_invaders_seen = 0;
}

// --- invader ---
struct Origin {
  bool valid = false;
  n::Vector3 pos{};
  float heading = 0;
  std::uint32_t health = 200, current_weapon = 0;
  struct Weapon { std::uint32_t id, ammo; };
  std::vector<Weapon> weapons;
};

enum class Stage { Idle, Probe, Node, Car, Await, Hunting };

bool g_active = false;
Origin g_origin;
Stage g_stage = Stage::Idle;
DWORD g_stage_since = 0, g_begin_ms = 0;
int g_attempts = 0;
n::Vector3 g_probe{}, g_spawn_at{};
float g_spawn_heading = 0;
n::Vehicle g_car = 0; // the FBI car we created
bool g_police_ignored = false;
std::optional<std::string> g_leave;
bool g_window_closed = false;
std::uint8_t g_result = 0;
std::mt19937 g_rng{std::random_device{}()};

float rnd(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g_rng); }

void set_stage(Stage s) { g_stage = s, g_stage_since = GetTickCount(); }

void leave(const std::string& why, const char* banner_text = nullptr, banner::Kind kind = banner::Kind::Defeat) {
  if (g_leave) return;
  g_leave = why;
  if (banner_text) banner::show(kind, banner_text);
  log::info("Invasion: leaving ({})", why);
}

const Player* find(const Frame& f, std::uint8_t id) {
  for (const Player& p : f.players)
    if (p.id == id) return &p;
  return nullptr;
}

// The lowest-id invader (us included) spawns the car; the rest ride in it.
std::uint8_t lead_id(const Frame& f) {
  std::uint8_t lead = f.my_id;
  for (const Player& p : f.players)
    if (p.role == net::Role::Invader && p.id < lead) lead = p.id;
  return lead;
}

void loadout(n::Ped me) {
  n::RemoveAllCharWeapons(me);
  n::GiveWeaponToChar(me, kWeaponCarbine, 300);
  n::GiveWeaponToChar(me, kWeaponShotgun, 60);
  n::GiveWeaponToChar(me, kWeaponPistol, 100);
  n::SetCurrentCharWeapon(me, kWeaponCarbine);
  n::SetCharHealth(me, 200);
}

void hunting(const Frame& f, n::Ped me) {
  loadout(me);
  set_stage(Stage::Hunting);
  phone::notify(std::format("Invasion: hunt down {}! Kill the target to win.", f.host_name.empty() ? "the host" : f.host_name));
  log::info("Invasion: on the hunt");
}

void invader_tick(const Frame& f) {
  if (g_leave) return;
  if (g_result == static_cast<std::uint8_t>(Result::TargetDied))
    return leave("Target eliminated: invasion successful! Back to your own game.", "TARGET ELIMINATED", banner::Kind::Victory);
  if (g_result == static_cast<std::uint8_t>(Result::InvadersDown))
    return leave("All invaders are down: the invasion failed. Back to your own game.", "INVASION FAILED");
  if (g_result == static_cast<std::uint8_t>(Result::WindowClosed) || g_window_closed)
    return leave("The invasion window closed. Back to your own game.");
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me || !n::DoesCharExist(me)) return;
  if (n::IsCharDead(me)) return leave("You were killed. Back to your own game.", "YOU DIED"); // downed is the usual end; this is a real death
  if (!f.ready) return;
  const DWORD now = GetTickCount();

  const Player* target = find(f, net::kHostId);
  if (target && target->dead && g_stage == Stage::Hunting)
    return leave("Target eliminated: invasion successful! Back to your own game.", "TARGET ELIMINATED", banner::Kind::Victory);
  if (!g_police_ignored) {
    invoke(kSetPoliceIgnorePlayer, n::GetPlayerId(), true); // FBI: the police don't bother us
    g_police_ignored = true;
  }

  switch (g_stage) {
    case Stage::Idle: {
      if (!target || !target->known_pos) {
        if (now - g_begin_ms > 30000) leave("Couldn't find the target. Back to your own game.");
        return;
      }
      g_attempts = 0;
      set_stage(lead_id(f) == f.my_id ? Stage::Probe : Stage::Await);
      break;
    }
    case Stage::Probe: {
      if (!target || !target->known_pos) return;
      const float angle = rnd(0.0f, 6.2831853f), radius = rnd(kMinSpawnM, kMaxSpawnM);
      g_probe = {target->pos.x + std::cos(angle) * radius, target->pos.y + std::sin(angle) * radius, target->pos.z + 2.0f};
      if (n::IsCharInAnyCar(me)) n::WarpCharFromCarToCoord(me, n::GetCharCoordinates(me));
      n::SetCharCoordinatesNoOffset(me, g_probe);
      n::FreezeCharPosition(me, true);
      invoke(kLoadScene, g_probe.x, g_probe.y, g_probe.z);
      ++g_attempts;
      set_stage(Stage::Node);
      break;
    }
    case Stage::Node: {
      if (!target || !target->known_pos) return;
      invoke(kRequestCollision, g_probe.x, g_probe.y, g_probe.z);
      n::Vector3 node{};
      float heading = 0;
      const bool found = n::GetClosestCarNodeWithHeading(g_probe, node, heading);
      const DWORD waited = now - g_stage_since;
      if (found) {
        const float from_probe = dist2d(node, g_probe), from_target = dist2d(node, target->pos);
        const bool good = from_probe < 150.0f && from_target >= 150.0f && from_target <= 550.0f;
        if (good || (waited > 1500 && g_attempts >= 4)) {
          g_spawn_at = node, g_spawn_heading = heading;
          set_stage(Stage::Car);
        } else if (waited > 1500) {
          set_stage(Stage::Probe); // water or a far node: another bearing
        }
      } else if (waited > 4000) {
        if (g_attempts < 4) {
          set_stage(Stage::Probe);
        } else {
          g_spawn_at = g_probe, g_spawn_heading = 0;
          set_stage(Stage::Car);
        }
      }
      break;
    }
    case Stage::Car: {
      static const char* const kModels[] = {"FBI", "POLICE", "WASHINGTON"};
      std::uint32_t model = 0;
      for (const char* name : kModels) {
        const std::uint32_t h = n::GetHashKey(name);
        if (n::IsModelInCdimage(h)) {
          model = h;
          break;
        }
      }
      if (!model) return leave("No car to spawn in. Back to your own game.");
      n::RequestModel(model);
      if (!n::HasModelLoaded(model)) {
        if (now - g_stage_since > 15000) leave("Couldn't load the FBI car. Back to your own game.");
        return;
      }
      g_car = n::CreateCar(model, g_spawn_at);
      n::MarkModelAsNoLongerNeeded(model);
      n::FreezeCharPosition(me, false);
      if (!g_car) return leave("Couldn't create the FBI car. Back to your own game.");
      invoke(kSetCarHeading, g_car, g_spawn_heading);
      n::WarpCharIntoCar(me, g_car);
      n::SetCarEngineOn(g_car, true, true);
      log::info("Invasion: FBI car at {:.0f} {:.0f} {:.0f}, {:.0f} m from the target", g_spawn_at.x, g_spawn_at.y,
                g_spawn_at.z, target ? dist2d(g_spawn_at, target->pos) : 0.0f);
      hunting(f, me);
      break;
    }
    case Stage::Await: {
      if (lead_id(f) == f.my_id) { // the lead left before spawning: we're it now
        g_attempts = 0;
        set_stage(Stage::Probe);
        break;
      }
      const Player* lead = find(f, lead_id(f));
      if (lead && lead->car && n::DoesVehicleExist(lead->car)) {
        const std::uint32_t seats = std::min<std::uint32_t>(n::GetMaximumNumberOfPassengers(lead->car), 8);
        for (std::uint32_t seat = 0; seat < seats; ++seat)
          if (n::IsCarPassengerSeatFree(lead->car, seat)) {
            if (n::IsCharInAnyCar(me)) n::WarpCharFromCarToCoord(me, n::GetCharCoordinates(me));
            n::WarpCharIntoCarAsPassenger(me, lead->car, seat);
            log::info("Invasion: riding in the lead invader's car (seat {})", seat);
            hunting(f, me);
            return;
          }
      }
      if (now - g_stage_since > 20000) { // the lead never showed up with a car: spawn our own
        log::warn("Invasion: no lead car after 20 s; spawning our own");
        g_attempts = 0;
        set_stage(Stage::Probe);
      }
      break;
    }
    case Stage::Hunting:
      break;
  }
}

} // namespace

void tick(const Frame& f, net::Session& net) {
  if (f.in_session && f.hosting)
    host_tick(f, net);
  else
    host_reset(net);
  if (f.in_session && f.my_role == net::Role::Invader && g_active) invader_tick(f);
}

void on_message(const net::GameMessage& m) {
  if (m.from != net::kHostId) return;
  net::Reader r(m.data);
  std::uint8_t type, sender, sub, value;
  if (!r.get(type) || !r.get(sender) || !r.get(sub) || !r.get(value)) return;
  if (sub == static_cast<std::uint8_t>(Sub::Open)) {
    if (g_active && value == 0) g_window_closed = true;
  } else if (sub == static_cast<std::uint8_t>(Sub::Result)) {
    if (g_active) {
      g_result = value;
    } else if (value == static_cast<std::uint8_t>(Result::TargetDied)) { // the invaded group (a co-op joiner)
      banner::show(banner::Kind::Defeat, "DEFEATED BY THE FBI");
    } else if (value == static_cast<std::uint8_t>(Result::InvadersDown)) {
      banner::show(banner::Kind::Victory, "FBI VANQUISHED");
    }
  } else if (sub == static_cast<std::uint8_t>(Sub::Arrived)) {
    if (!g_active) banner::show(banner::Kind::Invading, "FBI INVADING");
  }
}

bool open() { return g_open; }
bool active() { return g_active; }

void begin(n::Ped me) {
  g_active = true;
  g_stage = Stage::Idle;
  g_begin_ms = GetTickCount();
  g_car = 0;
  g_police_ignored = false;
  g_leave.reset();
  g_window_closed = false;
  g_result = 0;
  g_origin = {};
  if (!me || !n::DoesCharExist(me)) return;
  g_origin.valid = true;
  g_origin.pos = n::GetCharCoordinates(me);
  g_origin.heading = n::GetCharHeading(me);
  g_origin.health = std::max<std::uint32_t>(n::GetCharHealth(me), 150);
  g_origin.current_weapon = n::GetCurrentCharWeapon(me);
  for (std::uint32_t slot = 0; slot < kWeaponSlots; ++slot) {
    std::uint32_t weapon = 0, ammo = 0, ammo2 = 0;
    invoke(kGetCharWeaponInSlot, me, slot, &weapon, &ammo, &ammo2);
    if (weapon) g_origin.weapons.push_back({weapon, ammo});
  }
  log::info("Invasion: origin saved ({:.0f} {:.0f} {:.0f}, health {}, {} weapons)", g_origin.pos.x, g_origin.pos.y,
            g_origin.pos.z, g_origin.health, g_origin.weapons.size());
}

void finish() {
  if (!g_active) return;
  g_active = false;
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (g_police_ignored) invoke(kSetPoliceIgnorePlayer, n::GetPlayerId(), false);
  g_police_ignored = false;
  if (me && n::DoesCharExist(me)) {
    n::FreezeCharPosition(me, false);
    if (n::IsCharInAnyCar(me)) n::WarpCharFromCarToCoord(me, n::GetCharCoordinates(me));
    if (g_origin.valid) {
      n::SetCharCoordinatesNoOffset(me, g_origin.pos);
      n::SetCharHeading(me, g_origin.heading);
      n::SetCharHealth(me, g_origin.health);
      n::RemoveAllCharWeapons(me);
      for (const auto& w : g_origin.weapons) n::GiveWeaponToChar(me, w.id, w.ammo);
      if (g_origin.current_weapon) n::SetCurrentCharWeapon(me, g_origin.current_weapon);
    }
  }
  if (g_car && n::DoesVehicleExist(g_car)) n::DeleteCar(g_car);
  g_car = 0;
  g_origin = {};
  log::info("Invasion: back in our own game");
}

std::optional<std::string> take_leave() { return std::exchange(g_leave, std::nullopt); }

std::uint32_t invader_model() {
  static std::uint32_t model = 0;
  static bool looked = false;
  if (!looked) {
    const std::uint32_t h = n::GetHashKey("M_M_FBI");
    if (n::IsModelInCdimage(h)) model = h;
    looked = true;
  }
  return model;
}

} // namespace ivc::invasion
