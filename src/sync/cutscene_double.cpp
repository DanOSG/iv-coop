#include "sync/cutscene_double.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/log.h"
#include "sync/character.h"
#include "sync/player_state.h"

namespace ivc::sync::cutscene_double {
namespace n = natives;
namespace {

constexpr std::uint32_t kGetSticks = 0x4F7F4FAE, kKeyPressed = 0x5FA96262;
constexpr int kDikW = 0x11, kDikA = 0x1E, kDikS = 0x1F, kDikD = 0x20, kDikShift = 0x2A;
constexpr float kFarFromActor = 200.0f; // beyond this the world around the double isn't streamed: it stands still
constexpr DWORD kRetaskMs = 150;

n::Ped g_ped = 0;
DWORD g_last_task = 0;
bool g_moving = false;

bool key(int dik) { return n::invoke<bool>(kKeyPressed, dik); }

// Movement input in -1..1 (x right, y forward): the pad's left stick, else WASD.
void read_input(float& x, float& y, bool& fast) {
  std::int32_t lx = 0, ly = 0, rx = 0, ry = 0;
  n::invoke(kGetSticks, 0, &lx, &ly, &rx, &ry);
  x = std::clamp(lx / 127.0f, -1.0f, 1.0f), y = std::clamp(-ly / 127.0f, -1.0f, 1.0f); // stick up is negative
  fast = std::hypot(x, y) > 0.6f;
  if (std::hypot(x, y) < 0.2f) {
    x = (key(kDikD) ? 1.0f : 0.0f) - (key(kDikA) ? 1.0f : 0.0f);
    y = (key(kDikW) ? 1.0f : 0.0f) - (key(kDikS) ? 1.0f : 0.0f);
    fast = key(kDikShift) || x != 0.0f || y != 0.0f;
  }
}

} // namespace

bool active() { return g_ped != 0; }
n::Ped ped() { return g_ped && n::DoesCharExist(g_ped) ? g_ped : 0; }

bool begin() {
  if (g_ped) return true;
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me || !n::DoesCharExist(me)) return true; // nothing to stand in for
  const std::uint32_t model = n::GetCharModel(me);
  n::RequestModel(model);
  if (!n::HasModelLoaded(model)) return false;
  const n::Vector3 at = n::GetCharCoordinates(me);
  const float heading = n::GetCharHeading(me);
  const std::uint32_t health = n::GetCharHealth(me), armour = n::GetCharArmour(me);
  const std::uint32_t weapon = n::GetCurrentCharWeapon(me);
  const std::uint32_t ammo = weapon ? n::GetAmmoInCharWeapon(me, weapon) : 0;
  n::Ped d = n::CreateChar(n::kPedTypeCivMale, model, at);
  n::MarkModelAsNoLongerNeeded(model);
  if (!d) return false; // retry next frame
  n::SetCharDefaultComponentVariation(d);
  if (character::is_own_model(model)) character::apply_own(d);
  else
    for (std::uint32_t c = 0; c < n::kPedComponents; ++c)
      n::SetCharComponentVariation(d, c, n::GetCharDrawableVariation(me, c), n::GetCharTextureVariation(me, c));
  n::SetCharHeading(d, heading);
  n::SetCharMaxHealth(d, std::max<std::uint32_t>(health, 200));
  n::SetCharHealth(d, health);
  if (armour) n::AddArmourToChar(d, armour);
  if (weapon) n::GiveWeaponToChar(d, weapon, std::max<std::uint32_t>(ammo, 1)), n::SetCurrentCharWeapon(d, weapon);
  n::SetBlockingOfNonTemporaryEvents(d, true);
  n::SetCharKeepTask(d, true);
  n::SetPedDontDoEvasiveDives(d, true);
  n::SetCharInvincible(d, true); // the real character stays safe while the scene plays
  g_ped = d, g_moving = false, g_last_task = 0;
  log::info("Cutscene double: created at {:.1f} {:.1f} {:.1f}", at.x, at.y, at.z);
  return true;
}

void tick() {
  if (!g_ped) return;
  if (!n::DoesCharExist(g_ped)) { g_ped = 0; return; }
  const n::Ped actor = n::GetPlayerChar(n::GetPlayerId());
  const n::Vector3 pos = n::GetCharCoordinates(g_ped);
  const bool distant = actor && n::DoesCharExist(actor) &&
                   [&] { const n::Vector3 a = n::GetCharCoordinates(actor);
                         return std::hypot(a.x - pos.x, a.y - pos.y) > kFarFromActor; }();
  n::FreezeCharPosition(g_ped, distant); // scene streaming moved to the cutscene: don't fall through unloaded ground

  float ix = 0, iy = 0;
  bool fast = false;
  if (!distant) read_input(ix, iy, fast);
  if (std::hypot(ix, iy) < 0.2f) {
    if (g_moving) n::ClearCharTasks(g_ped);
    g_moving = false;
    return;
  }
  const DWORD now = GetTickCount();
  if (g_moving && now - g_last_task < kRetaskMs) return;
  // Relative to the camera the player is looking through.
  float yaw = 0;
  if (const n::Camera cam = n::GetGameCam()) yaw = n::GetCamRot(cam).z * std::numbers::pi_v<float> / 180.0f;
  const float fx = -std::sin(yaw), fy = std::cos(yaw), rx = std::cos(yaw), ry = std::sin(yaw);
  const float dx = fx * iy + rx * ix, dy = fy * iy + ry * ix;
  const float len = std::hypot(dx, dy);
  if (len < 0.01f) return;
  constexpr float kStep = 6.0f;
  n::TaskGoStraightToCoord(g_ped, {pos.x + dx / len * kStep, pos.y + dy / len * kStep, pos.z},
                           fast ? n::MoveState::Run : n::MoveState::Walk, 600);
  g_moving = true, g_last_task = now;
}

n::Vector3 end() {
  n::Vector3 at{};
  if (g_ped && n::DoesCharExist(g_ped)) {
    at = n::GetCharCoordinates(g_ped);
    n::DeleteChar(g_ped);
  } else if (const n::Ped me = n::GetPlayerChar(n::GetPlayerId()); me) {
    at = n::GetCharCoordinates(me);
  }
  g_ped = 0, g_moving = false;
  return at;
}

} // namespace ivc::sync::cutscene_double
