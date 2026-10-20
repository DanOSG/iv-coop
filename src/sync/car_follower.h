#pragma once
// Physics-follow for a car someone else simulates: the local copy stays a physical car
// (wheels roll, it reacts to traffic and walls) and is pushed onto the owner's track with
// APPLY_FORCE_TO_CAR each frame, snapping only when it falls too far behind. Used for
// players' stand-in cars (vehicle_sync) and followed mission cars (entity_sync).

#include <cstdint>

#include "game/natives.h"

namespace ivc::sync {

struct CarPose {
  float x = 0, y = 0, z = 0;
  float qx = 0, qy = 0, qz = 0, qw = 1; // as returned by GET_VEHICLE_QUATERNION
  float vx = 0, vy = 0, vz = 0;        // world velocity, m/s
  float speed = 0;                     // signed forward speed (negative when reversing)
};

// Signed forward speed from a velocity and orientation (for poses that don't carry it).
float forward_speed(const CarPose& p);

class CarFollower {
 public:
  std::uint32_t force_type = 1;
  float error_m = 0;    // horizontal distance from the target last frame
  std::uint32_t snaps = 0;

  // Game thread, script context. Once per frame while following.
  void drive(natives::Vehicle car, const CarPose& t, std::uint32_t now_ms);
  void snap(natives::Vehicle car, const CarPose& t);
  void reset(); // new car or a pause in following

  // Diagnostics (overlay).
  float gain() const { return gain_; }
  bool blocked() const { return blocked_s_ > 0.75f; }
  float steer() const { return steer_; }

 private:
  void drive_physics(natives::Vehicle car, const CarPose& t, float dt);

  natives::Vector3 last_vel_{};
  natives::Vector3 last_request_{}; // velocity change we asked for last frame (world, m/s)
  bool have_last_ = false;
  float gain_ = 1.0f;
  std::uint32_t last_drive_ms_ = 0;
  float blocked_s_ = 0; // time spent pushing without making progress
  float steer_ = 0;
  bool have_yaw_ = false;
  float last_target_yaw_ = 0; // radians, for the steering estimate
};

} // namespace ivc::sync
