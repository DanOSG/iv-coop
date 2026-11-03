#pragma once
// Networked-entity messages (M4), carried on the session's game-message lane.
//
//   EntityCreate  host -> all, reliable   : a new networked entity exists
//   EntityDelete  host -> all, reliable
//   EntityOwner   host -> all, reliable   : its sim owner changed
//   EntityState   sim owner -> all, unreliable: pose/velocity/health stream
//
// Every entity's *script owner* is the host (its copy is what story scripts read and
// write). Its *sim owner* is the player whose copy runs physics/AI, normally the nearest.

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "net/bytes.h"
#include "net/protocol.h"
#include "sync/snapshot_buffer.h"

namespace ivc::sync {

enum class GameMsg : std::uint8_t {
  EntityCreate = net::kFirstGameMsg, // 16
  EntityDelete = 17,
  EntityOwner = 18,
  EntityState = 19,
  Damage = 20, // attacker -> all, reliable: the target's owner applies it (M5)
};

// Damage dealt by the sender to a player or a networked entity, as detected on the
// sender's copy of the target. Only the target's owner applies it.
enum class DamageTarget : std::uint8_t { Player = 0, Entity = 1 };
struct Damage {
  DamageTarget kind = DamageTarget::Player;
  std::uint32_t target = 0; // player id or entity id
  std::uint16_t amount = 0;
  std::uint32_t weapon = 0;
};

enum class EntityType : std::uint8_t { Ped = 1, Vehicle = 2, Object = 3 };

struct EntityState {
  std::uint32_t time_ms = 0;
  std::uint16_t seq = 0;
  std::uint32_t id = 0;
  float x = 0, y = 0, z = 0;
  float vx = 0, vy = 0, vz = 0;
  float qx = 0, qy = 0, qz = 0, qw = 1; // vehicles/objects: full orientation; peds: yaw only
  std::uint16_t health = 200;

  // Peds: what they're doing, so a joiner's copy does the same (walk/run, gun, shots at the same target,
  // crouch, ragdoll, fire, seat).
  std::uint32_t weapon = 0;
  std::uint16_t shots = 0;           // cumulative rounds fired (wraps); copies fire the difference
  std::uint8_t flags = 0;            // kShooting | kRagdoll | kOnFire | kDucking
  float ax = 0, ay = 0, az = 0;      // aim point (world)
  std::uint8_t target = 0xFF;        // the player it's shooting at (copies aim at their copy of them)
  std::uint32_t vehicle = 0;         // networked vehicle it sits in (0: on foot)
  std::int8_t seat = -1;             // -1 driver, 0.. passenger seats
  static constexpr std::uint8_t kShooting = 1, kRagdoll = 2, kOnFire = 4, kDucking = 8;

  static EntityState lerp(const EntityState& a, const EntityState& b, float k) {
    EntityState o = b;
    o.x = a.x + (b.x - a.x) * k, o.y = a.y + (b.y - a.y) * k, o.z = a.z + (b.z - a.z) * k;
    // Precise entities (EntitySync::kPreciseBit, an activity's physics object): a cubic Hermite through both
    // snapshots' positions and velocities, so a ball's path between frames curves the way it moved.
    if ((b.id & 0x400000) && !(b.id & 0xFF800000u) && b.time_ms != a.time_ms) {
      const float t = static_cast<float>(b.time_ms - a.time_ms) / 1000.0f;
      const float k2 = k * k, k3 = k2 * k;
      const float h00 = 2 * k3 - 3 * k2 + 1, h10 = k3 - 2 * k2 + k, h01 = -2 * k3 + 3 * k2, h11 = k3 - k2;
      o.x = h00 * a.x + h10 * t * a.vx + h01 * b.x + h11 * t * b.vx;
      o.y = h00 * a.y + h10 * t * a.vy + h01 * b.y + h11 * t * b.vy;
      o.z = h00 * a.z + h10 * t * a.vz + h01 * b.z + h11 * t * b.vz;
    }
    const float dot = a.qx * b.qx + a.qy * b.qy + a.qz * b.qz + a.qw * b.qw;
    const float s = dot < 0 ? -1.0f : 1.0f;
    o.qx = a.qx + (s * b.qx - a.qx) * k, o.qy = a.qy + (s * b.qy - a.qy) * k;
    o.qz = a.qz + (s * b.qz - a.qz) * k, o.qw = a.qw + (s * b.qw - a.qw) * k;
    const float len = std::sqrt(o.qx * o.qx + o.qy * o.qy + o.qz * o.qz + o.qw * o.qw);
    if (len > 1e-6f) o.qx /= len, o.qy /= len, o.qz /= len, o.qw /= len;
    return o;
  }
  static EntityState extrapolate(const EntityState& s, float dt) {
    EntityState o = s;
    o.x += s.vx * dt, o.y += s.vy * dt, o.z += s.vz * dt;
    return o;
  }

  // Peds: heading (degrees, IV convention) <-> yaw quaternion about +Z.
  static void set_heading(EntityState& s, float heading_deg) {
    const float h = heading_deg * 3.14159265f / 180.0f;
    s.qx = s.qy = 0, s.qz = std::sin(h / 2), s.qw = std::cos(h / 2);
  }
  float heading() const {
    float h = std::atan2(2.0f * (qw * qz + qx * qy), 1.0f - 2.0f * (qy * qy + qz * qz)) * 180.0f / 3.14159265f;
    return h < 0 ? h + 360.0f : h;
  }
};

using EntityInterpolator = SnapshotBuffer<EntityState>;

struct EntityCreate {
  std::uint32_t id = 0;
  EntityType type = EntityType::Ped;
  std::uint32_t model = 0;
  float x = 0, y = 0, z = 0;
  float qx = 0, qy = 0, qz = 0, qw = 1;
  std::uint8_t sim_owner = 0;
};

// --- encoding: [type][sender (stamped by the session)][payload] ---

inline std::vector<std::uint8_t> encode_create(const EntityCreate& c) {
  net::Writer w;
  w.put(GameMsg::EntityCreate).put(std::uint8_t{0}).put(c.id).put(c.type).put(c.model).put(c.x).put(c.y).put(c.z)
      .put(c.qx).put(c.qy).put(c.qz).put(c.qw).put(c.sim_owner);
  return std::move(w.data());
}

inline std::vector<std::uint8_t> encode_delete(std::uint32_t id) {
  net::Writer w;
  w.put(GameMsg::EntityDelete).put(std::uint8_t{0}).put(id);
  return std::move(w.data());
}

inline std::vector<std::uint8_t> encode_owner(std::uint32_t id, std::uint8_t sim_owner) {
  net::Writer w;
  w.put(GameMsg::EntityOwner).put(std::uint8_t{0}).put(id).put(sim_owner);
  return std::move(w.data());
}

inline std::vector<std::uint8_t> encode_entity_state(const EntityState& s) {
  net::Writer w;
  w.put(GameMsg::EntityState).put(std::uint8_t{0}).put(s.id).put(s.seq).put(s.time_ms).put(s.x).put(s.y).put(s.z)
      .put(s.vx).put(s.vy).put(s.vz).put(s.qx).put(s.qy).put(s.qz).put(s.qw).put(s.health).put(s.weapon).put(s.shots)
      .put(s.flags).put(s.ax).put(s.ay).put(s.az).put(s.target).put(s.vehicle).put(s.seat);
  return std::move(w.data());
}

inline std::vector<std::uint8_t> encode_damage(const Damage& d) {
  net::Writer w;
  w.put(GameMsg::Damage).put(std::uint8_t{0}).put(d.kind).put(d.target).put(d.amount).put(d.weapon);
  return std::move(w.data());
}

// Decoded form of any entity/combat message.
struct EntityMsg {
  GameMsg type{};
  std::uint8_t sender = 0;
  EntityCreate create;    // EntityCreate
  std::uint32_t id = 0;   // EntityDelete / EntityOwner
  std::uint8_t owner = 0; // EntityOwner
  EntityState state;      // EntityState
  Damage damage;          // Damage
};

inline std::optional<EntityMsg> decode_entity_msg(std::span<const std::uint8_t> data) {
  net::Reader r(data);
  EntityMsg m;
  r.get(m.type), r.get(m.sender);
  switch (m.type) {
    case GameMsg::EntityCreate: {
      auto& c = m.create;
      r.get(c.id), r.get(c.type), r.get(c.model), r.get(c.x), r.get(c.y), r.get(c.z);
      r.get(c.qx), r.get(c.qy), r.get(c.qz), r.get(c.qw), r.get(c.sim_owner);
      break;
    }
    case GameMsg::EntityDelete:
      r.get(m.id);
      break;
    case GameMsg::EntityOwner:
      r.get(m.id), r.get(m.owner);
      break;
    case GameMsg::EntityState: {
      auto& s = m.state;
      r.get(s.id), r.get(s.seq), r.get(s.time_ms), r.get(s.x), r.get(s.y), r.get(s.z), r.get(s.vx), r.get(s.vy);
      r.get(s.vz), r.get(s.qx), r.get(s.qy), r.get(s.qz), r.get(s.qw), r.get(s.health);
      r.get(s.weapon), r.get(s.shots), r.get(s.flags), r.get(s.ax), r.get(s.ay), r.get(s.az), r.get(s.target);
      r.get(s.vehicle), r.get(s.seat);
      break;
    }
    case GameMsg::Damage:
      r.get(m.damage.kind), r.get(m.damage.target), r.get(m.damage.amount), r.get(m.damage.weapon);
      break;
    default:
      return std::nullopt;
  }
  if (!r.ok()) return std::nullopt;
  return m;
}

} // namespace ivc::sync
