#include "sync/puppet.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "game/player_slots.h"

namespace ivc::sync {
namespace n = natives;
namespace {
constexpr std::uint32_t kUnlockRagdoll = 0x2F2F51E9, kIsPedRagdoll = 0x3E251ADE, kSwitchPedToRagdoll = 0x1A0F56C5,
                        kAllowReactionAnims = 0x0FEA6230,
                        kClearCharTasksImmediately = 0x3C116620, kIsCharGettingUp = 0x4A906237, kIsCharOnFire = 0x358E21C5, kStartCharFire = 0x5FB31295, kExtinguishCharFire = 0x5D786EEE;

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
constexpr std::uint32_t kPartnerBlipColour = 2;

float angle_diff(float a, float b) { return std::fabs(std::fmod(a - b + 540.0f, 360.0f) - 180.0f); }

n::MoveState faster(n::MoveState m) {
  return m == n::MoveState::Walk ? n::MoveState::Run : n::MoveState::Sprint;
}

} // namespace

bool Puppet::ensure(const n::Vector3& at) {
  if (ped_ && n::DoesCharExist(ped_)) return true;
  if (ped_) {
    log::warn("Puppet ped {} vanished; respawning", ped_);
    player_slots::release(ped_);
    ped_ = 0;
    blip_ = 0; // the game removes blips attached to deleted peds
    targetable_ = true;
  }
  model_ = model_override_ ? model_override_ : n::GetHashKey(model_name_.c_str());
  n::RequestModel(model_);
  if (!n::HasModelLoaded(model_)) {
    phase_ = "loading model";
    return false;
  }
  // A player ped (an extra player slot) reacts to hits like a player; only it can wear the protagonist
  // model. Out of slots, other models fall back to an ordinary ped.
  ped_ = animated_ ? player_slots::acquire(at, 0.0f, model_) : 0;
  is_player_ped_ = ped_ != 0;
  if (!ped_ && model_ == player_slots::kProtagonistModel) {
    phase_ = "no player slot";
    return false;
  }
  if (!ped_) {
    ped_ = n::CreateChar(n::kPedTypeCivMale, model_, at);
    n::MarkModelAsNoLongerNeeded(model_);
    if (!ped_) {
      phase_ = "CREATE_CHAR failed";
      return false;
    }
  }
  n::SetBlockingOfNonTemporaryEvents(ped_, true);
  // Not invincible (hits must register so we can report them), but on a big health buffer
  // that is refilled every frame, so a local hit can never kill it: only its owner decides.
  n::SetCharMaxHealth(ped_, kPuppetHealthBuffer);
  n::SetCharHealth(ped_, kPuppetHealthBuffer);
  shown_health_ = kPuppetHealthBuffer;
  n::SetCharAccuracy(ped_, 100); // its shots go where the remote player aimed
  dead_ = false, weapon_ = 0, have_shots_ = false, outfit_applied_ = false;
  if (animated_) {
    // A real, physical ped that walks on the ground and keeps the tasks we give it.
    n::SetCharKeepTask(ped_, true);
    n::SetPedDontDoEvasiveDives(ped_, true);
    n::SetDontActivateRagdollFromPlayerImpact(ped_, true);
    n::SetCharWillMoveWhenInjured(ped_, true);
    // Hits don't knock a partner over here; they ragdoll when their owner does (mirror_body). A player
    // ped already shrugs off hits like a player; an ordinary ped is ragdoll-locked and has its reaction
    // anims off (and take_hits_from cancels canned knockdowns).
    mirroring_ragdoll_ = false;
    if (!is_player_ped_) {
      n::invoke(kUnlockRagdoll, ped_, false);
      n::invoke(kAllowReactionAnims, ped_, false);
      ragdoll_locked_ = true;
    } else {
      ragdoll_locked_ = false;
    }
  } else {
    n::SetCharCollision(ped_, false);
    n::FreezeCharPosition(ped_, true);
  }
  blip_ = n::AddBlipForChar(ped_);
  blip_visible_ = true;
  // Friendly green (IV: 1 red, 2 green, 3 blue, 5 yellow), not the default red that reads
  // as an enemy and not blue/yellow, which missions use for targets and destinations.
  if (blip_) n::ChangeBlipColour(blip_, kPartnerBlipColour);
  downed_marker_ = false;
  action_ = Action::None;
  phase_ = "spawned";
  log::info("Puppet spawned: ped {} model {} (0x{:08X}), {}", ped_, model_override_ ? "(peer's)" : model_name_.c_str(), model_,
            animated_ ? "animated" : "frozen");
  return true;
}

void Puppet::snap_to(const PlayerState& t) {
  n::SetCharCoordinatesNoOffset(ped_, {t.x, t.y, t.z});
  n::SetCharHeading(ped_, t.heading);
  action_ = Action::None; // the teleport cancels whatever we were doing
  ++snaps_;
}

void Puppet::mirror_body(bool ragdoll, bool on_fire, std::uint32_t now_ms) {
  if (!ped_ || dead_ || !n::DoesCharExist(ped_)) return;
  const bool is_ragdoll = n::invoke<bool>(kIsPedRagdoll, ped_);
  if (ragdoll) {
    // Keep it down for as long as the owner is; re-trigger when the engine stands it up.
    if (!mirroring_ragdoll_ || (!is_ragdoll && now_ms - ragdoll_since_ > 300)) {
      n::invoke(kUnlockRagdoll, ped_, true);
      n::invoke(kSwitchPedToRagdoll, ped_, 0, 1500, 1, 1, 1, 0);
      mirroring_ragdoll_ = true, ragdoll_locked_ = false, ragdoll_since_ = now_ms;
    }
  } else if (mirroring_ragdoll_ || (!ragdoll_locked_ && !is_player_ped_)) {
    if (!is_ragdoll) {
      if (!is_player_ped_) n::invoke(kUnlockRagdoll, ped_, false), ragdoll_locked_ = true;
      mirroring_ragdoll_ = false;
    }
  }
  const bool burning = n::invoke<bool>(kIsCharOnFire, ped_);
  if (on_fire && !burning) n::invoke(kStartCharFire, ped_);
  else if (!on_fire && burning) n::invoke(kExtinguishCharFire, ped_);
}

void Puppet::unlock_ragdoll() {
  if (!ped_ || !ragdoll_locked_) return;
  n::invoke(kUnlockRagdoll, ped_, true);
  ragdoll_locked_ = false;
}

void Puppet::drive(const PlayerState& t, std::uint32_t now_ms) {
  if (!ped_) return;
  if (mirroring_ragdoll_ || (!ragdoll_locked_ && n::invoke<bool>(kIsPedRagdoll, ped_))) {
    phase_ = "ragdoll";
    return; // tasks would fight the ragdoll; it gets up when the owner does
  }
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
  // Every reissue restarts the task (and, for the aiming one, its aim-up animation), so the
  // aiming variant aims further ahead and is refreshed much less eagerly.
  const bool aiming = (t.flags & PlayerState::kAiming) && t.weapon;
  const n::Vector3 aim{t.ax, t.ay, t.az};
  const float regoal = aiming ? 2.5f : kRegoalDist;
  const std::uint32_t reissue = aiming ? 1200 : kReissueMs;
  const float aim_moved = std::hypot(aim.x - last_aim_.x, aim.y - last_aim_.y, aim.z - last_aim_.z);
  if (action_ != Action::Moving || ms != move_ || goal_moved > regoal || now_ms - last_issue_ms_ > reissue ||
      aiming != moving_aimed_ || (aiming && aim_moved > 3.0f)) {
    if (aiming) { // gun up while moving: walk/run and aim at their aim point at the same time
      const n::Vector3 far_goal{t.x + t.vx * 1.5f, t.y + t.vy * 1.5f, t.z};
      n::TaskGoToCoordWhileAiming(ped_, far_goal, ms, aim);
      last_aim_ = aim;
    } else {
      n::TaskGoStraightToCoord(ped_, goal, ms, 2000);
    }
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

void Puppet::set_model(std::uint32_t model_hash) {
  // The protagonist model ("player") is spawned as an extra player ped (see ensure).
  // Anything that is not a ped model in the image (a bad hash from a peer) would crash
  // CREATE_CHAR, so it falls back to the default.
  if (model_hash && model_hash != player_slots::kProtagonistModel &&
      !(n::IsModelInCdimage(model_hash) && n::IsThisModelAPed(model_hash))) {
    if (model_hash != rejected_model_) log::info("Puppet model 0x{:08X} can't be a ped; using the default", model_hash);
    rejected_model_ = model_hash;
    model_hash = 0;
  }
  if (model_hash == model_override_) return;
  model_override_ = model_hash;
  if (ped_) {
    log::info("Puppet model -> 0x{:08X}; respawning", model_hash);
    despawn(); // ensure() spawns the new model next frame
  }
}

void Puppet::apply_outfit(const PlayerState& s) {
  if (!ped_ || dead_) return;
  for (std::uint32_t c = 0; c < n::kPedComponents; ++c) {
    if (outfit_applied_ && worn_drawable_[c] == s.drawable[c] && worn_texture_[c] == s.texture[c]) continue;
    n::SetCharComponentVariation(ped_, c, s.drawable[c], s.texture[c]);
    worn_drawable_[c] = s.drawable[c], worn_texture_[c] = s.texture[c];
  }
  outfit_applied_ = true;
}

std::uint16_t Puppet::take_hits_from(n::Ped attacker, bool cancel_knockdowns) {
  if (!ped_ || dead_ || !n::DoesCharExist(ped_)) return 0;
  const std::uint32_t h = n::GetCharHealth(ped_);
  const std::uint32_t lost = h < shown_health_ ? shown_health_ - h : 0;
  const bool by_attacker = attacker && n::HasCharBeenDamagedByChar(ped_, attacker);
  if (lost) n::SetCharHealth(ped_, shown_health_);
  // With its ragdoll locked, a hit that would knock it over plays a canned fall instead; cancel whatever the
  // hit started this frame (before it shows) and let drive() hand it its movement again.
  if (cancel_knockdowns && animated_ && !is_player_ped_ && !mirroring_ragdoll_ && !vehicle_ && // (clearing would pull it out of a car)
      (lost || n::invoke<bool>(kIsCharGettingUp, ped_))) {
    n::invoke(kClearCharTasksImmediately, ped_);
    action_ = Action::None;
  }
  n::ClearCharLastDamageEntity(ped_);
  return by_attacker ? static_cast<std::uint16_t>(std::min<std::uint32_t>(lost, 0xFFFF)) : 0;
}

void Puppet::apply_combat(const PlayerState& s, std::uint32_t now_ms) {
  if (!ped_) return;

  // Death: follow the owner's state. Dying here is cosmetic (ragdoll); when they're alive
  // again (respawned at a hospital) the dead puppet is replaced by a fresh one.
  if (s.flags & PlayerState::kDead) {
    if (!dead_) {
      unlock_ragdoll(); // a death falls like one
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

  // Our copy knocked over locally (e.g. hit by a car here): no shooting or aiming visuals
  // until it's back up. Shots during that time are dropped, not saved up; the real damage
  // still comes from the shooter's own Damage messages.
  const bool down = n::IsPedRagdoll(ped_) || n::IsCharGettingUp(ped_);

  // Shots: fire the difference of their cumulative counter (wraps at 16 bits), so lost
  // packets never lose shots. Cap per frame so a long stall doesn't empty a magazine.
  const n::Vector3 aim{s.ax, s.ay, s.az};
  if (have_shots_ && weapon_ && !down) {
    const std::uint16_t diff = static_cast<std::uint16_t>(s.shots - last_shots_);
    if (diff > 0 && diff < 200) {
      for (std::uint16_t i = 0; i < std::min<std::uint16_t>(diff, 5); ++i) n::FirePedWeapon(ped_, aim);
      n::SetCharAmmo(ped_, weapon_, 9999);
    }
  }
  last_shots_ = s.shots, have_shots_ = true;

  // Aiming while standing (on foot): keep the aim task fresh while their gun is up. While
  // moving, drive() already uses the move-while-aiming task.
  // Long task, refreshed only when it's about to run out or the aim point really moved
  // (reissuing restarts the aim-up animation, which reads as jitter).
  const bool standing = std::hypot(s.vx, s.vy) < kStillSpeed;
  const float aim_moved = std::hypot(aim.x - last_aim_.x, aim.y - last_aim_.y, aim.z - last_aim_.z);
  if ((s.flags & PlayerState::kAiming) && weapon_ && !vehicle_ && standing && !down &&
      (action_ != Action::Aiming || now_ms - last_aim_ms_ > 2500 || aim_moved > 2.0f)) {
    n::TaskAimGunAtCoord(ped_, aim, 3000);
    last_aim_ms_ = now_ms, last_aim_ = aim;
    action_ = Action::Aiming;
  }
}

void Puppet::set_downed_marker(bool downed) {
  if (downed == downed_marker_ || !blip_) return;
  downed_marker_ = downed;
  n::ChangeBlipColour(blip_, downed ? 1 : kPartnerBlipColour);
  n::invoke(0x4DFE09D6, blip_, downed); // FLASH_BLIP
}

void Puppet::apply_settings(bool targetable, bool blip_visible) {
  if (!ped_) return;
  if (targetable != targetable_) {
    n::invoke(0x5EA84115, ped_, !targetable); // SET_CHAR_NEVER_TARGETTED
    targetable_ = targetable;
  }
  if (blip_ && blip_visible != blip_visible_) {
    n::ChangeBlipDisplay(blip_, blip_visible ? 2 : 0);
    blip_visible_ = blip_visible;
  }
}

void Puppet::show_health(std::uint32_t health) {
  if (!ped_ || dead_) return;
  // Player health runs 100 (dead) .. 200. Scale that onto the buffer so the lock-on health bar (health /
  // max) reads the same, but never below kMinShown: one local hit must not kill the copy here.
  constexpr std::uint32_t kMinShown = 300;
  const std::uint32_t above = health > 100 ? std::min<std::uint32_t>(health - 100, 100) : 0;
  const std::uint32_t shown = std::max(kMinShown, 100 + above * (kPuppetHealthBuffer - 100) / 100);
  if (shown == shown_health_) return;
  shown_health_ = shown;
  n::SetCharHealth(ped_, shown);
}

void Puppet::despawn() {
  if (blip_) n::RemoveBlip(blip_), blip_ = 0;
  if (player_slots::owns(ped_)) player_slots::release(ped_);
  else if (ped_ && n::DoesCharExist(ped_)) n::DeleteChar(ped_);
  ped_ = 0;
  vehicle_ = 0;
  targetable_ = blip_visible_ = true;
  action_ = Action::None;
  dead_ = false;
  phase_ = "idle";
}

} // namespace ivc::sync
