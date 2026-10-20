#include "sync/puppet.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"

namespace ivc::sync {
namespace n = natives;
namespace {

// Tuning for animated mode.
constexpr float kSnapDist = 5.0f;       // m of horizontal error before we teleport
constexpr float kSnapDz = 4.0f;         // m of vertical error before we teleport (floors, falls)
constexpr float kStillSpeed = 0.5f;     // m/s: below this the remote player is standing
constexpr float kSettleDist = 0.6f;     // m: standing remote, but we're not on the spot yet
constexpr float kWalkMax = 2.5f;        // m/s: walk below, run above
constexpr float kRunMax = 5.5f;         // m/s: run below, sprint above
constexpr float kCatchUpDist = 2.0f;    // m behind: move one gait faster
constexpr float kLeadSeconds = 0.6f;    // aim this far ahead along the remote's velocity
constexpr float kRegoalDist = 0.75f;    // m the goal must move before we reissue the task
constexpr std::uint32_t kReissueMs = 300;
constexpr float kTurnThresholdDeg = 15.0f;

float angle_diff(float a, float b) { return std::fabs(std::fmod(a - b + 540.0f, 360.0f) - 180.0f); }

n::MoveState faster(n::MoveState m) {
  return m == n::MoveState::Walk ? n::MoveState::Run : n::MoveState::Sprint;
}

} // namespace

bool Puppet::ensure(const n::Vector3& at) {
  if (ped_ && n::DoesCharExist(ped_)) return true;
  if (ped_) {
    log::warn("Puppet ped {} vanished; respawning", ped_);
    ped_ = 0;
    blip_ = 0; // the game removes blips attached to deleted peds
  }
  if (!model_) model_ = n::GetHashKey(model_name_.c_str());
  n::RequestModel(model_);
  if (!n::HasModelLoaded(model_)) {
    phase_ = "loading model";
    return false;
  }
  ped_ = n::CreateChar(n::kPedTypeCivMale, model_, at);
  n::MarkModelAsNoLongerNeeded(model_);
  if (!ped_) {
    phase_ = "CREATE_CHAR failed";
    return false;
  }
  n::SetBlockingOfNonTemporaryEvents(ped_, true);
  // Not invincible (hits must register so we can report them), but on a big health buffer
  // that is refilled every frame, so a local hit can never kill it: only its owner decides.
  n::SetCharMaxHealth(ped_, kPuppetHealthBuffer);
  n::SetCharHealth(ped_, kPuppetHealthBuffer);
  n::SetCharAccuracy(ped_, 100); // its shots go where the remote player aimed
  dead_ = false, weapon_ = 0, have_shots_ = false;
  if (animated_) {
    // A real, physical ped that walks on the ground and keeps the tasks we give it.
    n::SetCharKeepTask(ped_, true);
    n::SetPedDontDoEvasiveDives(ped_, true);
    n::SetDontActivateRagdollFromPlayerImpact(ped_, true);
    n::SetCharWillMoveWhenInjured(ped_, true);
  } else {
    n::SetCharCollision(ped_, false);
    n::FreezeCharPosition(ped_, true);
  }
  blip_ = n::AddBlipForChar(ped_);
  action_ = Action::None;
  phase_ = "spawned";
  log::info("Puppet spawned: ped {} model {} (0x{:08X}), {}", ped_, model_name_, model_,
            animated_ ? "animated" : "frozen");
  return true;
}

void Puppet::snap_to(const PlayerState& t) {
  n::SetCharCoordinatesNoOffset(ped_, {t.x, t.y, t.z});
  n::SetCharHeading(ped_, t.heading);
  action_ = Action::None; // the teleport cancels whatever we were doing
  ++snaps_;
}

void Puppet::drive(const PlayerState& t, std::uint32_t now_ms) {
  if (!ped_) return;
  if (!animated_) {
    n::SetCharCoordinatesNoOffset(ped_, {t.x, t.y, t.z});
    n::SetCharHeading(ped_, t.heading);
    phase_ = "frozen";
    return;
  }
  drive_animated(t, now_ms);
}

void Puppet::drive_animated(const PlayerState& t, std::uint32_t now_ms) {
  if (dead_) return;
  const n::Vector3 cur = n::GetCharCoordinates(ped_);
  const float dx = t.x - cur.x, dy = t.y - cur.y;
  error_m_ = std::hypot(dx, dy);

  if (error_m_ > kSnapDist || std::fabs(t.z - cur.z) > kSnapDz) {
    snap_to(t);
    phase_ = "snapped";
    return;
  }

  const float speed = std::hypot(t.vx, t.vy);

  // Standing with a gun up: the aim task (apply_combat) owns the ped; don't fight it.
  if ((t.flags & PlayerState::kAiming) && t.weapon && speed < kStillSpeed && error_m_ < kSettleDist) {
    phase_ = "aiming";
    return;
  }

  // --- remote player is standing still ---
  if (speed < kStillSpeed) {
    n::SetCharMoveAnimSpeedMultiplier(ped_, 1.0f);
    if (error_m_ > kSettleDist) {
      // Walk the last bit onto their spot.
      if (action_ != Action::Moving || now_ms - last_issue_ms_ > kReissueMs) {
        n::TaskGoStraightToCoord(ped_, {t.x, t.y, t.z}, n::MoveState::Walk, 2000);
        action_ = Action::Moving, move_ = n::MoveState::Walk, last_issue_ms_ = now_ms;
      }
      phase_ = "settling";
      return;
    }
    if (action_ != Action::Standing || angle_diff(t.heading, last_heading_) > kTurnThresholdDeg) {
      n::ClearCharTasks(ped_);
      n::TaskAchieveHeading(ped_, t.heading);
      action_ = Action::Standing, last_heading_ = t.heading;
    }
    phase_ = "standing";
    return;
  }

  // --- remote player is moving: pick the gait from their speed ---
  n::MoveState ms = speed < kWalkMax ? n::MoveState::Walk : speed < kRunMax ? n::MoveState::Run : n::MoveState::Sprint;
  if (error_m_ > kCatchUpDist && ms != n::MoveState::Sprint) ms = faster(ms);

  // Aim ahead along their velocity, so the task never runs out before we refresh it.
  const n::Vector3 goal{t.x + t.vx * kLeadSeconds, t.y + t.vy * kLeadSeconds, t.z};
  const float goal_moved = std::hypot(goal.x - last_goal_.x, goal.y - last_goal_.y);
  const bool aiming = (t.flags & PlayerState::kAiming) && t.weapon;
  if (action_ != Action::Moving || ms != move_ || goal_moved > kRegoalDist || now_ms - last_issue_ms_ > kReissueMs ||
      aiming != moving_aimed_) {
    if (aiming) // gun up while moving: walk/run and aim at their aim point at the same time
      n::TaskGoToCoordWhileAiming(ped_, goal, ms, {t.ax, t.ay, t.az});
    else
      n::TaskGoStraightToCoord(ped_, goal, ms, 2000);
    action_ = Action::Moving, move_ = ms, last_goal_ = goal, last_issue_ms_ = now_ms, moving_aimed_ = aiming;
  }

  // Fine speed match: hurry if the true position is ahead of us along the direction of
  // travel, ease off if we've overtaken it.
  const float along = (dx * t.vx + dy * t.vy) / speed;
  n::SetCharMoveAnimSpeedMultiplier(ped_, std::clamp(1.0f + along * 0.15f, 0.8f, 1.3f));

  phase_ = ms == n::MoveState::Walk ? "walking" : ms == n::MoveState::Run ? "running" : "sprinting";
}

bool Puppet::sit_in(n::Vehicle vehicle, std::int8_t seat) {
  if (!ped_ || !vehicle) return false;
  if (vehicle_ == vehicle && seat_ == seat && n::IsCharInCar(ped_, vehicle)) return true;
  if (!animated_) {
    // Frozen puppets can't be seated; thaw first.
    n::FreezeCharPosition(ped_, false);
    n::SetCharCollision(ped_, true);
  }
  n::ClearCharTasks(ped_);
  if (seat == n::kDriverSeat)
    n::WarpCharIntoCar(ped_, vehicle);
  else
    n::WarpCharIntoCarAsPassenger(ped_, vehicle, static_cast<std::uint32_t>(seat));
  const bool ok = n::IsCharInCar(ped_, vehicle);
  if (ok) {
    vehicle_ = vehicle, seat_ = seat;
    action_ = Action::None;
    phase_ = seat == n::kDriverSeat ? "driving" : "passenger";
  } else {
    phase_ = "seat taken";
  }
  return ok;
}

void Puppet::leave_vehicle(const n::Vector3& at) {
  if (!ped_ || !vehicle_) return;
  n::WarpCharFromCarToCoord(ped_, at);
  vehicle_ = 0, seat_ = -1;
  action_ = Action::None;
  if (!animated_) {
    n::SetCharCollision(ped_, false);
    n::FreezeCharPosition(ped_, true);
  }
}

std::uint16_t Puppet::take_hits_from(n::Ped attacker) {
  if (!ped_ || dead_ || !n::DoesCharExist(ped_)) return 0;
  const std::uint32_t h = n::GetCharHealth(ped_);
  const std::uint32_t lost = h < kPuppetHealthBuffer ? kPuppetHealthBuffer - h : 0;
  const bool by_attacker = attacker && n::HasCharBeenDamagedByChar(ped_, attacker);
  if (lost) n::SetCharHealth(ped_, kPuppetHealthBuffer);
  n::ClearCharLastDamageEntity(ped_);
  return by_attacker ? static_cast<std::uint16_t>(std::min<std::uint32_t>(lost, 0xFFFF)) : 0;
}

void Puppet::apply_combat(const PlayerState& s, std::uint32_t now_ms) {
  if (!ped_) return;

  // Death: follow the owner's state. Dying here is cosmetic (ragdoll); when they're alive
  // again (respawned at a hospital) the dead puppet is replaced by a fresh one.
  if (s.flags & PlayerState::kDead) {
    if (!dead_) {
      n::SetCharHealth(ped_, 0);
      dead_ = true;
      phase_ = "dead";
    }
    return;
  }
  if (dead_ || n::IsCharDead(ped_)) {
    despawn(); // ensure() respawns it next frame at their current position
    return;
  }

  // Weapon in hand.
  if (s.weapon != weapon_) {
    if (s.weapon) n::GiveWeaponToChar(ped_, s.weapon, 9999);
    n::SetCurrentCharWeapon(ped_, s.weapon);
    weapon_ = s.weapon;
  }

  // Shots: fire the difference of their cumulative counter (wraps at 16 bits), so lost
  // packets never lose shots. Cap per frame so a long stall doesn't empty a magazine.
  const n::Vector3 aim{s.ax, s.ay, s.az};
  if (have_shots_ && weapon_) {
    const std::uint16_t diff = static_cast<std::uint16_t>(s.shots - last_shots_);
    if (diff > 0 && diff < 200) {
      for (std::uint16_t i = 0; i < std::min<std::uint16_t>(diff, 5); ++i) n::FirePedWeapon(ped_, aim);
      n::SetCharAmmo(ped_, weapon_, 9999);
    }
  }
  last_shots_ = s.shots, have_shots_ = true;

  // Aiming while standing (on foot): keep the aim task fresh while their gun is up. While
  // moving, drive() already uses the move-while-aiming task.
  const bool standing = std::hypot(s.vx, s.vy) < kStillSpeed;
  if ((s.flags & PlayerState::kAiming) && weapon_ && !vehicle_ && standing && now_ms - last_aim_ms_ > 250) {
    n::TaskAimGunAtCoord(ped_, aim, 600);
    last_aim_ms_ = now_ms;
    action_ = Action::None; // movement re-issues its task when they move again
  }
}

void Puppet::despawn() {
  if (blip_) n::RemoveBlip(blip_), blip_ = 0;
  if (ped_ && n::DoesCharExist(ped_)) n::DeleteChar(ped_);
  ped_ = 0;
  vehicle_ = 0;
  action_ = Action::None;
  dead_ = false;
  phase_ = "idle";
}

} // namespace ivc::sync
