#include "sync/car_follower.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ivc::sync {
namespace n = natives;
namespace {
constexpr float kSnapDist = 10.0f; // m of error before the car is teleported instead of steered
} // namespace

float forward_speed(const CarPose& p) {
  // The car's forward axis (+Y) rotated by its orientation.
  const float fx = 2.0f * (p.qx * p.qy - p.qw * p.qz);
  const float fy = 1.0f - 2.0f * (p.qx * p.qx + p.qz * p.qz);
  const float fz = 2.0f * (p.qy * p.qz + p.qw * p.qx);
  return p.vx * fx + p.vy * fy + p.vz * fz;
}

void CarFollower::reset() {
  have_last_ = false;
  have_yaw_ = false;
  blocked_s_ = 0;
  last_drive_ms_ = 0;
}

void CarFollower::snap(n::Vehicle car, const CarPose& t) {
  n::SetCarCoordinatesNoOffset(car, {t.x, t.y, t.z});
  n::SetVehicleQuaternion(car, {t.qx, t.qy, t.qz, t.qw});
  n::SetCarForwardSpeed(car, t.speed);
  have_last_ = false; // the teleport invalidates the velocity measurement
  blocked_s_ = 0;
  ++snaps;
}

void CarFollower::drive(n::Vehicle car, const CarPose& t, std::uint32_t now_ms) {
  const n::Vector3 cur = n::GetCarCoordinates(car);
  error_m = std::hypot(t.x - cur.x, t.y - cur.y);
  const float dt = last_drive_ms_ ? std::clamp((now_ms - last_drive_ms_) / 1000.0f, 0.004f, 0.05f) : 1.0f / 60;
  last_drive_ms_ = now_ms;
  drive_physics(car, t, dt);
}

// Each frame we request the velocity change that brings the car onto the owner's track
// (their velocity plus a pull toward their position) through APPLY_FORCE_TO_CAR with mass
// scaling. Which force type is an impulse isn't known statically, so the controller
// measures how much velocity change it actually got per m/s requested (horizontal only,
// gravity-free) and scales its requests by that.
void CarFollower::drive_physics(n::Vehicle car, const CarPose& t, float dt) {
  constexpr float kPull = 2.0f;      // 1/s: position error -> corrective velocity
  constexpr float kMaxPull = 8.0f;   // m/s cap on the corrective part
  // Authority: a car-like 2 g. Enough to hold a fast circle, but weak enough that walls
  // and other cars win the contact, so the car stops/deflects instead of being bulldozed
  // through (it then snaps once it's kSnapDist behind).
  constexpr float kMaxAccel = 20.0f; // m/s^2
  constexpr float kSnapDz = 4.0f;
  constexpr float kRotBlend = 0.15f; // fraction of the orientation error closed per frame

  const n::Vector3 cur = n::GetCarCoordinates(car);
  if (error_m > kSnapDist || std::fabs(t.z - cur.z) > kSnapDz) {
    snap(car, t);
    return;
  }
  const n::Vector3 vel = n::GetCarSpeedVector(car);

  // Learn the gain from last frame's request (horizontal, only when the request was big
  // enough to measure over noise from friction, collisions and the engine).
  if (have_last_) {
    const float rx = last_request_.x, ry = last_request_.y;
    const float req2 = rx * rx + ry * ry;
    if (req2 > 0.01f) { // requests are per-frame sized (<= 2 g * dt), so a low floor
      const float got = ((vel.x - last_vel_.x) * rx + (vel.y - last_vel_.y) * ry) / req2;
      if (got > 0.001f && got < 3.0f) gain_ += (got - gain_) * 0.1f;
    }
  }

  // Steering: turn the front wheels by the owner's yaw rate (bicycle model) so they point
  // into curves. (Tight turns still screech, as a real car would.)
  const float yaw = std::atan2(2.0f * (t.qw * t.qz + t.qx * t.qy), 1.0f - 2.0f * (t.qy * t.qy + t.qz * t.qz));
  if (have_yaw_) {
    float dyaw = yaw - last_target_yaw_;
    if (dyaw > std::numbers::pi_v<float>) dyaw -= 2 * std::numbers::pi_v<float>;
    if (dyaw < -std::numbers::pi_v<float>) dyaw += 2 * std::numbers::pi_v<float>;
    const float rate = dyaw / dt;
    constexpr float kWheelbase = 2.7f, kMaxSteer = 0.6f; // m, rad
    const float speed = std::max(std::fabs(t.speed), 2.0f);
    const float target_steer = std::clamp(std::atan(kWheelbase * rate / speed) / kMaxSteer, -1.0f, 1.0f);
    steer_ += (target_steer - steer_) * 0.2f; // yaw rate from 30 Hz snapshots is jumpy
  }
  have_yaw_ = true;
  last_target_yaw_ = yaw;
  n::SetVehicleSteerBias(car, steer_);

  // Horizontal only: gravity and suspension keep the car on the road; height mismatches
  // beyond kSnapDz are fixed by snapping.
  const float ex = t.x - cur.x, ey = t.y - cur.y;
  const float pull = std::hypot(ex, ey) * kPull;
  const float pull_scale = pull > kMaxPull ? kMaxPull / pull : 1.0f;
  const float dvx = t.vx + ex * kPull * pull_scale - vel.x;
  const float dvy = t.vy + ey * kPull * pull_scale - vel.y;
  const float dv = std::hypot(dvx, dvy);
  const float max_dv = kMaxAccel * dt;
  const float dv_scale = dv > max_dv ? max_dv / dv : 1.0f;
  const n::Vector3 want{dvx * dv_scale, dvy * dv_scale, 0.0f};

  // Blocked: well off track and not moving toward it (pinned against a wall, or stuck
  // under traffic). Keep pushing and the solver lets us creep through the geometry; so
  // stop pushing, sit there like a stuck car would, and let the snap catch up.
  constexpr float kBlockedErr = 3.0f, kBlockedAfter = 0.75f;
  const float err = std::hypot(ex, ey);
  const float toward = err > 0.01f ? (vel.x * ex + vel.y * ey) / err : 0.0f;
  if (err > kBlockedErr && toward < 1.5f)
    blocked_s_ += dt;
  else
    blocked_s_ = std::max(0.0f, blocked_s_ - 2 * dt);
  if (blocked_s_ > kBlockedAfter) {
    have_last_ = false;
    return; // no force, no rotation: let the contact hold
  }

  const float g = std::max(gain_, 0.005f);
  n::ApplyForceToCar(car, force_type, {want.x / g, want.y / g, want.z / g});
  last_vel_ = vel;
  last_request_ = want;
  have_last_ = true;

  // Ease the orientation toward the owner's (short-arc nlerp of the current one).
  // (Tried and dropped: re-asserting forward speed each frame to "sync" wheel spin; it
  // overwrites vertical velocity and fights the suspension, so the car sinks.)
  const n::Vector4 q = n::GetVehicleQuaternion(car);
  const float dot = q.x * t.qx + q.y * t.qy + q.z * t.qz + q.w * t.qw;
  const float s = dot < 0 ? -1.0f : 1.0f;
  n::Vector4 o{q.x + (s * t.qx - q.x) * kRotBlend, q.y + (s * t.qy - q.y) * kRotBlend,
               q.z + (s * t.qz - q.z) * kRotBlend, q.w + (s * t.qw - q.w) * kRotBlend};
  const float len = std::sqrt(o.x * o.x + o.y * o.y + o.z * o.z + o.w * o.w);
  if (len > 1e-6f) n::SetVehicleQuaternion(car, {o.x / len, o.y / len, o.z / len, o.w / len});
}

} // namespace ivc::sync
