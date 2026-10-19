#pragma once
// Replicated state of a vehicle, sent by its sim owner (normally the driving player).

#include <cmath>
#include <cstdint>

#include "sync/snapshot_buffer.h"

namespace ivc::sync {

struct VehicleState {
  std::uint32_t time_ms = 0;
  std::uint16_t seq = 0;
  std::uint32_t vehicle_id = 0; // owner-assigned: (owner player id << 24) | counter
  std::uint32_t model = 0;
  float x = 0, y = 0, z = 0;
  float qx = 0, qy = 0, qz = 0, qw = 1; // orientation as returned by GET_VEHICLE_QUATERNION
  float vx = 0, vy = 0, vz = 0;        // world velocity, m/s
  float speed = 0;                     // signed forward speed (negative when reversing)
  std::int8_t colour[4] = {0, 0, 0, 0};
  std::uint16_t health = 1000;
  float engine_health = 1000;
  std::uint8_t siren = 0;

  static VehicleState lerp(const VehicleState& a, const VehicleState& b, float k) {
    VehicleState o = b;
    o.x = a.x + (b.x - a.x) * k;
    o.y = a.y + (b.y - a.y) * k;
    o.z = a.z + (b.z - a.z) * k;
    // Normalised lerp on the shorter arc: close enough to slerp at 30 Hz snapshot spacing.
    const float dot = a.qx * b.qx + a.qy * b.qy + a.qz * b.qz + a.qw * b.qw;
    const float s = dot < 0 ? -1.0f : 1.0f;
    o.qx = a.qx + (s * b.qx - a.qx) * k;
    o.qy = a.qy + (s * b.qy - a.qy) * k;
    o.qz = a.qz + (s * b.qz - a.qz) * k;
    o.qw = a.qw + (s * b.qw - a.qw) * k;
    const float len = std::sqrt(o.qx * o.qx + o.qy * o.qy + o.qz * o.qz + o.qw * o.qw);
    if (len > 1e-6f) o.qx /= len, o.qy /= len, o.qz /= len, o.qw /= len;
    o.vx = a.vx + (b.vx - a.vx) * k;
    o.vy = a.vy + (b.vy - a.vy) * k;
    o.vz = a.vz + (b.vz - a.vz) * k;
    o.speed = a.speed + (b.speed - a.speed) * k;
    return o;
  }
  static VehicleState extrapolate(const VehicleState& s, float dt) {
    VehicleState o = s;
    o.x += s.vx * dt, o.y += s.vy * dt, o.z += s.vz * dt;
    return o;
  }
};

using VehicleInterpolator = SnapshotBuffer<VehicleState>;

} // namespace ivc::sync
