#pragma once
// Networked entities (M4): script owner (always the host) vs sim owner (nearest player).
//
// Every machine holds a local copy of each networked entity:
//   - on the sim owner it is *live*: physics and AI run normally, and its state is
//     streamed out;
//   - everywhere else it is a *follower*: frozen and posed from the interpolated stream.
// The host's copy is the one story scripts see. When a client simulates the entity, the
// host's copy is a follower, frozen and placed on the synced pose, so it is correct for
// scripts even where the host has no collision streamed.
//
// The host decides sim ownership: nearest player, with hysteresis, re-evaluated twice a
// second, and reassigned immediately when an owner leaves. Migration hands the new owner
// the last synced pose; its copy wakes up from there.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "game/natives.h"
#include "net/session.h"
#include "sync/entity_protocol.h"

namespace ivc::sync {

struct PlayerPos {
  std::uint8_t id;
  natives::Vector3 pos;
};

struct EntityStatus {
  std::uint32_t id;
  EntityType type;
  std::uint8_t sim_owner;
  bool spawned;
  bool live; // our copy is authoritative
  float x, y, z;
  std::uint32_t migrations;
};

class EntitySync {
 public:
  std::uint32_t send_hz = 15;
  std::uint32_t interp_delay_ms = 100;
  float handover_margin_m = 25.0f; // another player must be this much closer to take over

  // --- host only ---
  // Makes an existing local entity networked (we become its sim owner). Returns its id.
  std::optional<std::uint32_t> host_register(natives::Handle handle, EntityType type, std::uint32_t model,
                                             net::Session& net);
  void host_delete_all(net::Session& net); // removes every networked entity everywhere
  void on_player_joined(std::uint8_t id, net::Session& net); // late joiner: resend creates/owners
  void on_player_left(std::uint8_t id);                       // its entities get reassigned next evaluation

  // --- everyone ---
  void on_message(const net::GameMessage& m, std::uint8_t my_id);
  void tick(std::uint32_t now_ms, std::uint8_t my_id, bool hosting, const std::vector<PlayerPos>& players,
            net::Session& net);
  // Session ended: clients delete their copies; the host keeps its entities as plain
  // local ones (unfrozen, live).
  void clear(bool was_host);

  std::vector<EntityStatus> status() const;

  // --- combat (M5) ---
  // Damage our player did to follower copies since the last call (copies sit on a
  // refilled health buffer, so local hits never kill them; the sim owner decides).
  std::vector<Damage> collect_hits(natives::Ped attacker);
  // Damage reported against an entity: applied only if we are its sim owner.
  void apply_damage(std::uint32_t id, std::uint16_t amount, std::uint8_t my_id);

 private:
  struct Entity {
    std::uint32_t id = 0;
    EntityType type = EntityType::Ped;
    std::uint32_t model = 0;
    std::uint8_t sim_owner = 0;
    natives::Handle handle = 0;
    bool live = false; // current mode of our local copy
    EntityInterpolator interp;
    EntityState last{}; // latest pose we know (ours if live, else latest received)
    std::uint16_t seq = 0;
    std::uint32_t last_send_ms = 0;
    std::uint32_t migrations = 0;
    bool shown_dead = false; // follower: owner reported death, we killed our copy
  };

  void apply_create(const EntityCreate& c, std::uint8_t my_id);
  void set_owner(Entity& e, std::uint8_t owner, std::uint8_t my_id);
  bool ensure_spawned(Entity& e, std::uint8_t my_id);
  void make_live(Entity& e);
  void make_follower(Entity& e);
  EntityState read_local(const Entity& e, std::uint32_t now_ms);
  void pose_follower(Entity& e, const EntityState& s);
  void evaluate_ownership(const std::vector<PlayerPos>& players, std::uint8_t my_id, net::Session& net);
  void destroy_copy(Entity& e);

  std::map<std::uint32_t, Entity> ents_;
  std::uint32_t next_id_ = 1;
  std::uint32_t last_eval_ms_ = 0;
};

} // namespace ivc::sync
