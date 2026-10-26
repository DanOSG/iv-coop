#pragma once
#include <cmath>
#include <cstdint>

#include "sync/snapshot_buffer.h"

namespace ivc::sync {

inline constexpr std::uint32_t kNoVehicle = 0;

// Player snapshot. Grows as sync covers more (pad state, aim, weapon).
// Wire encoding lives in net/protocol.h.
struct PlayerState {
  std::uint32_t time_ms = 0; // sender clock when captured
  std::uint16_t seq = 0;
  float x = 0, y = 0, z = 0;
  float vx = 0, vy = 0, vz = 0;
  float heading = 0; // degrees, 0..360
  std::uint32_t model = 0;
  std::uint8_t in_vehicle = 0;
  // Which networked vehicle the player is in (kNoVehicle if on foot) and which seat:
  // -1 driver, 0.. passenger seats.
  std::uint32_t vehicle_id = kNoVehicle;
  std::int8_t seat = -1;

  // Combat (M5).
  std::uint16_t health = 200;
  std::uint16_t armour = 0;
  std::uint32_t weapon = 0;  // weapon type in hand (0 = unarmed)
  std::uint16_t shots = 0;   // cumulative shots fired (wraps); receivers fire the difference
  float ax = 0, ay = 0, az = 0; // aim point (world)
  std::uint8_t flags = 0;    // kAiming | kDead | kDowned | kRagdoll | kOnFire
  std::uint8_t wanted = 0;   // wanted level 0-6

  static constexpr std::uint8_t kAiming = 1;
  static constexpr std::uint8_t kDead = 2;
  static constexpr std::uint8_t kDowned = 4; // down, waiting for a revive (sync/downed)
  static constexpr std::uint8_t kRagdoll = 8; // knocked over (vehicle, explosion, fall...): copies ragdoll too
  static constexpr std::uint8_t kOnFire = 16;

  // Outfit (M6d): drawable + texture variation for each of the 11 ped components.
  std::uint8_t drawable[11] = {};
  std::uint8_t texture[11] = {};

  static float lerp_angle(float a, float b, float k) {
    float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
    return std::fmod(a + d * k + 360.0f, 360.0f);
  }
  static PlayerState lerp(const PlayerState& a, const PlayerState& b, float k) {
    PlayerState o = b; // discrete fields (vehicle, seat, model) take the newer value
    o.x = a.x + (b.x - a.x) * k;
    o.y = a.y + (b.y - a.y) * k;
    o.z = a.z + (b.z - a.z) * k;
    o.heading = lerp_angle(a.heading, b.heading, k);
    return o;
  }
  static PlayerState extrapolate(const PlayerState& s, float dt) {
    PlayerState o = s;
    o.x += s.vx * dt, o.y += s.vy * dt, o.z += s.vz * dt;
    return o;
  }
};

using Interpolator = SnapshotBuffer<PlayerState>;

} // namespace ivc::sync
