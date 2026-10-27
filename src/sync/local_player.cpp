#include "sync/local_player.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "game/natives.h"
#include "sync/cutscene_double.h"
#include "sync/downed.h"

namespace ivc::sync {
namespace n = natives;
namespace {

// Shot counting: a shot is one round leaving the gun, i.e. total ammo for the same weapon
// going down (reloads move rounds between clip and reserve without changing the total).
std::uint32_t g_last_weapon = 0;
std::uint32_t g_last_ammo = 0;
std::uint16_t g_shots = 0;
std::uint32_t g_last_shot_ms = 0;

n::Vector3 aim_point(n::Ped me) {
  const n::Camera cam = n::GetGameCam();
  if (cam) {
    const n::Vector3 p = n::GetCamPos(cam), r = n::GetCamRot(cam);
    const float pitch = r.x * std::numbers::pi_v<float> / 180.0f, yaw = r.z * std::numbers::pi_v<float> / 180.0f;
    constexpr float kRange = 60.0f;
    return {p.x - std::sin(yaw) * std::cos(pitch) * kRange, p.y + std::cos(yaw) * std::cos(pitch) * kRange,
            p.z + std::sin(pitch) * kRange};
  }
  return n::GetOffsetFromCharInWorldCoords(me, {0.0f, 30.0f, 0.5f}); // no camera: straight ahead
}

} // namespace

n::Ped capture_ped() {
  // During a replicated cutscene the player ped is the scripted protagonist; the joiner's own
  // character is the cutscene double.
  if (const n::Ped d = cutscene_double::ped()) return d;
  return n::GetPlayerChar(n::GetPlayerId());
}

std::optional<PlayerState> capture_local(std::uint32_t now_ms) {
  const n::Player player = n::GetPlayerId();
  const n::Ped me = capture_ped();
  if (!me || !n::DoesCharExist(me)) return std::nullopt;

  PlayerState s;
  const n::Vector3 pos = n::GetCharCoordinates(me);
  const n::Vector3 vel = n::GetCharVelocity(me);
  s.time_ms = now_ms;
  s.x = pos.x, s.y = pos.y, s.z = pos.z;
  s.vx = vel.x, s.vy = vel.y, s.vz = vel.z;
  s.heading = n::GetCharHeading(me);
  s.model = n::GetCharModel(me);
  s.in_vehicle = n::IsCharInAnyCar(me);

  s.health = static_cast<std::uint16_t>(std::min<std::uint32_t>(n::GetCharHealth(me), 0xFFFF));
  s.armour = static_cast<std::uint16_t>(std::min<std::uint32_t>(n::GetCharArmour(me), 0xFFFF));
  s.weapon = n::GetCurrentCharWeapon(me);
  const std::uint32_t ammo = s.weapon ? n::GetAmmoInCharWeapon(me, s.weapon) : 0;
  if (s.weapon && s.weapon == g_last_weapon && ammo < g_last_ammo) {
    g_shots = static_cast<std::uint16_t>(g_shots + std::min<std::uint32_t>(g_last_ammo - ammo, 10));
    g_last_shot_ms = now_ms;
  }
  g_last_weapon = s.weapon, g_last_ammo = ammo;
  s.shots = g_shots;

  const n::Vector3 aim = aim_point(me);
  s.ax = aim.x, s.ay = aim.y, s.az = aim.z;
  if (n::IsPlayerTargettingAnything(player) || now_ms - g_last_shot_ms < 1500) s.flags |= PlayerState::kAiming;
  if (n::IsPlayerDead(player) || n::IsCharDead(me)) s.flags |= PlayerState::kDead;
  if (downed::local_downed()) s.flags |= PlayerState::kDowned;
  if (n::invoke<bool>(0x3E251ADE, me)) s.flags |= PlayerState::kRagdoll; // IS_PED_RAGDOLL
  if (n::invoke<bool>(0x358E21C5, me)) s.flags |= PlayerState::kOnFire;  // IS_CHAR_ON_FIRE
  s.wanted = static_cast<std::uint8_t>(std::min<std::uint32_t>(n::StoreWantedLevel(player), 6));
  for (std::uint32_t c = 0; c < n::kPedComponents; ++c) {
    s.drawable[c] = static_cast<std::uint8_t>(n::GetCharDrawableVariation(me, c));
    s.texture[c] = static_cast<std::uint8_t>(n::GetCharTextureVariation(me, c));
  }
  return s;
}

} // namespace ivc::sync
