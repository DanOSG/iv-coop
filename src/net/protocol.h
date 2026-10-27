#pragma once
// Wire protocol. Every packet is [MsgType u8][payload], little-endian.
// Channel 0 = unreliable (player state), channel 1 = reliable (session control).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "net/bytes.h"
#include "sync/player_state.h"
#include "sync/vehicle_state.h"

namespace ivc::net {

// 2: vehicle id/seat in PlayerState, VehicleState. 3: game-message lane (entities).
// 4: combat fields in PlayerState. 5: outfit in PlayerState. 6: roles (co-op / invader) in Hello and the roster.
inline constexpr std::uint16_t kProtocolVersion = 6;

// Message types >= kFirstGameMsg are "game messages": [type][sender id][payload]. The
// session layer doesn't interpret them; the host stamps the sender id and relays them to
// every other client with the reliability they arrived with. Gameplay systems (entities,
// combat, story events, ...) define their own types in this range.
inline constexpr std::uint8_t kFirstGameMsg = 16;
inline constexpr int kMaxCoop = 4;     // the co-op group: host + 3
inline constexpr int kMaxInvaders = 4; // an invading group
inline constexpr int kMaxPlayers = kMaxCoop + kMaxInvaders; // host + 7 while invaders are in
inline constexpr std::uint8_t kHostId = 0;
inline constexpr std::size_t kNameLen = 24;

inline constexpr std::uint8_t kChanState = 0;
inline constexpr std::uint8_t kChanControl = 1;
inline constexpr std::uint8_t kNumChannels = 2;
// ENet MTU: Steam's unreliable P2P packets (the relay path) are limited to 1200 bytes.
inline constexpr std::uint32_t kMtu = 1200;

// Disconnect reasons (ENet disconnect data).
enum class Reject : std::uint32_t { None = 0, Full = 1, VersionMismatch = 2, HostLeft = 3, Kicked = 4, NotOpen = 5 };

// What a player is in the session. Invaders (M11) join a co-op group's session to hunt its host's protagonist.
enum class Role : std::uint8_t { Coop = 0, Invader = 1 };

enum class MsgType : std::uint8_t {
  Hello = 1,        // C->H: version, name
  Welcome = 2,      // H->C: your id, roster
  PlayerJoined = 3, // H->all: id, name
  PlayerLeft = 4,   // H->all: id
  PlayerState = 5,  // any: id (stamped by host when relaying), state
  VehicleState = 6, // sim owner: id (stamped by host when relaying), state
};

using Name = std::array<char, kNameLen>;

inline Name make_name(const std::string& s) {
  Name n{};
  std::memcpy(n.data(), s.data(), std::min(s.size(), kNameLen - 1));
  return n;
}
inline std::string to_string(const Name& n) { return std::string(n.data(), strnlen(n.data(), kNameLen)); }

struct RosterEntry {
  std::uint8_t id;
  Name name;
  Role role = Role::Coop;
};

inline std::vector<std::uint8_t> encode_hello(const std::string& name, Role role = Role::Coop) {
  Writer w;
  w.put(MsgType::Hello).put(kProtocolVersion).put(make_name(name)).put(role);
  return std::move(w.data());
}

inline std::vector<std::uint8_t> encode_welcome(std::uint8_t your_id, const std::vector<RosterEntry>& roster) {
  Writer w;
  w.put(MsgType::Welcome).put(your_id).put(static_cast<std::uint8_t>(roster.size()));
  for (const auto& r : roster) w.put(r.id).put(r.name).put(r.role);
  return std::move(w.data());
}

inline std::vector<std::uint8_t> encode_joined(std::uint8_t id, const Name& name, Role role = Role::Coop) {
  Writer w;
  w.put(MsgType::PlayerJoined).put(id).put(name).put(role);
  return std::move(w.data());
}

inline std::vector<std::uint8_t> encode_left(std::uint8_t id) {
  Writer w;
  w.put(MsgType::PlayerLeft).put(id);
  return std::move(w.data());
}

inline std::vector<std::uint8_t> encode_state(std::uint8_t id, const sync::PlayerState& s) {
  Writer w;
  w.put(MsgType::PlayerState).put(id).put(s.seq).put(s.time_ms).put(s.x).put(s.y).put(s.z).put(s.vx).put(s.vy)
      .put(s.vz).put(s.heading).put(s.model).put(s.in_vehicle).put(s.vehicle_id).put(s.seat).put(s.health)
      .put(s.armour).put(s.weapon).put(s.shots).put(s.ax).put(s.ay).put(s.az).put(s.flags).put(s.wanted)
      .put(s.drawable).put(s.texture);
  return std::move(w.data());
}

inline bool decode_state(Reader& r, std::uint8_t& id, sync::PlayerState& s) {
  r.get(id), r.get(s.seq), r.get(s.time_ms), r.get(s.x), r.get(s.y), r.get(s.z), r.get(s.vx), r.get(s.vy);
  r.get(s.vz), r.get(s.heading), r.get(s.model), r.get(s.in_vehicle), r.get(s.vehicle_id), r.get(s.seat);
  r.get(s.health), r.get(s.armour), r.get(s.weapon), r.get(s.shots), r.get(s.ax), r.get(s.ay), r.get(s.az);
  r.get(s.flags), r.get(s.wanted), r.get(s.drawable), r.get(s.texture);
  return r.ok();
}

inline std::vector<std::uint8_t> encode_vehicle(std::uint8_t id, const sync::VehicleState& v) {
  Writer w;
  w.put(MsgType::VehicleState).put(id).put(v.seq).put(v.time_ms).put(v.vehicle_id).put(v.model).put(v.x).put(v.y)
      .put(v.z).put(v.qx).put(v.qy).put(v.qz).put(v.qw).put(v.vx).put(v.vy).put(v.vz).put(v.speed).put(v.colour)
      .put(v.health).put(v.engine_health).put(v.siren);
  return std::move(w.data());
}

inline bool decode_vehicle(Reader& r, std::uint8_t& id, sync::VehicleState& v) {
  r.get(id), r.get(v.seq), r.get(v.time_ms), r.get(v.vehicle_id), r.get(v.model), r.get(v.x), r.get(v.y), r.get(v.z);
  r.get(v.qx), r.get(v.qy), r.get(v.qz), r.get(v.qw), r.get(v.vx), r.get(v.vy), r.get(v.vz), r.get(v.speed);
  r.get(v.colour), r.get(v.health), r.get(v.engine_health), r.get(v.siren);
  return r.ok();
}

// Rewrites the id byte of an encoded PlayerState/VehicleState in place (host relay).
inline void restamp_state(std::span<std::uint8_t> packet, std::uint8_t id) {
  if (packet.size() > 1) packet[1] = id;
}

} // namespace ivc::net
