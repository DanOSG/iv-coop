#include "sync/entity_sync.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/log.h"

namespace ivc::sync {
namespace n = natives;
namespace {

constexpr std::uint32_t kEvalIntervalMs = 500;
constexpr std::uint32_t kPedBuffer = 2000;   // follower ped health buffer
constexpr std::uint32_t kCarBuffer = 1000;   // follower vehicle health buffer (cars burn below ~250)

bool exists(EntityType t, n::Handle h) {
  if (!h) return false;
  return t == EntityType::Ped ? n::DoesCharExist(h) : n::DoesVehicleExist(h);
}

float dist(const EntityState& s, const n::Vector3& p) { return std::hypot(s.x - p.x, s.y - p.y, s.z - p.z); }

} // namespace

// ---------------------------------------------------------------- host API

std::optional<std::uint32_t> EntitySync::host_register(n::Handle handle, EntityType type, std::uint32_t model,
                                                       net::Session& net) {
  if (!exists(type, handle)) return std::nullopt;
  const std::uint32_t id = next_id_++ & 0xFFFFFF; // host-minted (top byte 0 = host)
  Entity& e = ents_[id];
  e.id = id, e.type = type, e.model = model, e.sim_owner = net::kHostId, e.handle = handle, e.live = true;
  e.last = read_local(e, net::steady_ms());

  EntityCreate c;
  c.id = id, c.type = type, c.model = model, c.x = e.last.x, c.y = e.last.y, c.z = e.last.z;
  c.qx = e.last.qx, c.qy = e.last.qy, c.qz = e.last.qz, c.qw = e.last.qw, c.sim_owner = net::kHostId;
  net.send_game(encode_create(c), true);
  log::info("Entity {} registered: {} model 0x{:08X} handle {}", id, type == EntityType::Ped ? "ped" : "vehicle", model,
            handle);
  return id;
}

void EntitySync::host_delete_all(net::Session& net) {
  for (auto& [id, e] : ents_) {
    net.send_game(encode_delete(id), true);
    destroy_copy(e);
  }
  log::info("Deleted {} networked entities", ents_.size());
  ents_.clear();
}

void EntitySync::on_player_joined(std::uint8_t player, net::Session& net) {
  // Broadcast is fine: creates are idempotent on clients that already have the entity.
  for (const auto& [id, e] : ents_) {
    EntityCreate c;
    c.id = id, c.type = e.type, c.model = e.model, c.x = e.last.x, c.y = e.last.y, c.z = e.last.z;
    c.qx = e.last.qx, c.qy = e.last.qy, c.qz = e.last.qz, c.qw = e.last.qw, c.sim_owner = e.sim_owner;
    net.send_game(encode_create(c), true);
  }
  if (!ents_.empty()) log::info("Sent {} entities to late joiner #{}", ents_.size(), player);
}

void EntitySync::on_player_left(std::uint8_t player) {
  // Their entities keep their last pose; evaluate_ownership() reassigns them on the next
  // pass because the owner is no longer among the players.
  (void)player;
  last_eval_ms_ = 0;
}

// ---------------------------------------------------------------- messages

void EntitySync::apply_create(const EntityCreate& c, std::uint8_t my_id) {
  auto [it, inserted] = ents_.try_emplace(c.id);
  Entity& e = it->second;
  if (inserted) {
    e.id = c.id, e.type = c.type, e.model = c.model, e.sim_owner = c.sim_owner;
    e.interp.delay_ms = interp_delay_ms;
    e.last.id = c.id, e.last.x = c.x, e.last.y = c.y, e.last.z = c.z;
    e.last.qx = c.qx, e.last.qy = c.qy, e.last.qz = c.qz, e.last.qw = c.qw;
    log::info("Entity {} created by host ({} model 0x{:08X}), sim owner #{}", c.id,
              c.type == EntityType::Ped ? "ped" : "vehicle", c.model, c.sim_owner);
  } else {
    set_owner(e, c.sim_owner, my_id);
  }
}

void EntitySync::on_message(const net::GameMessage& m, std::uint8_t my_id) {
  const auto msg = decode_entity_msg(m.data);
  if (!msg) return;
  switch (msg->type) {
    case GameMsg::EntityCreate:
      if (m.from == net::kHostId) apply_create(msg->create, my_id);
      break;
    case GameMsg::EntityDelete:
      if (m.from == net::kHostId) {
        if (auto it = ents_.find(msg->id); it != ents_.end()) {
          destroy_copy(it->second);
          ents_.erase(it);
        }
      }
      break;
    case GameMsg::EntityOwner:
      if (m.from == net::kHostId) {
        if (auto it = ents_.find(msg->id); it != ents_.end()) set_owner(it->second, msg->owner, my_id);
      }
      break;
    case GameMsg::EntityState:
      if (auto it = ents_.find(msg->state.id); it != ents_.end()) {
        Entity& e = it->second;
        // Only the current sim owner's stream counts (late packets from a previous owner
        // after a migration are dropped).
        if (m.from == e.sim_owner && e.sim_owner != my_id) {
          e.interp.push(msg->state, m.arrival_ms);
          e.last = msg->state;
        }
      }
      break;
  }
}

// ---------------------------------------------------------------- ownership

void EntitySync::set_owner(Entity& e, std::uint8_t owner, std::uint8_t my_id) {
  if (owner == e.sim_owner) return;
  log::info("Entity {}: sim owner #{} -> #{}", e.id, e.sim_owner, owner);
  e.sim_owner = owner;
  ++e.migrations;
  e.interp.clear(); // new owner, new timeline
  if (!exists(e.type, e.handle)) return; // mode is applied when it spawns
  if (owner == my_id)
    make_live(e);
  else
    make_follower(e);
}

void EntitySync::evaluate_ownership(const std::vector<PlayerPos>& players, std::uint8_t my_id, net::Session& net) {
  if (players.empty()) return;
  for (auto& [id, e] : ents_) {
    const PlayerPos* nearest = nullptr;
    float d_near = std::numeric_limits<float>::max();
    float d_cur = std::numeric_limits<float>::max();
    for (const auto& p : players) {
      const float d = dist(e.last, p.pos);
      if (d < d_near) d_near = d, nearest = &p;
      if (p.id == e.sim_owner) d_cur = d;
    }
    const bool owner_present = d_cur != std::numeric_limits<float>::max();
    std::uint8_t next = e.sim_owner;
    if (!owner_present)
      next = nearest->id; // owner left: hand it to whoever is nearest, now
    else if (nearest->id != e.sim_owner && d_near + handover_margin_m < d_cur)
      next = nearest->id; // clearly closer: migrate
    if (next != e.sim_owner) {
      set_owner(e, next, my_id);
      net.send_game(encode_owner(e.id, next), true);
    }
  }
}

// ---------------------------------------------------------------- local copies

bool EntitySync::ensure_spawned(Entity& e, std::uint8_t my_id) {
  if (exists(e.type, e.handle)) return true;
  if (e.handle) {
    log::warn("Entity {}: local copy {} vanished; respawning", e.id, e.handle);
    e.handle = 0;
  }
  n::RequestModel(e.model);
  if (!n::HasModelLoaded(e.model)) return false;
  const n::Vector3 at{e.last.x, e.last.y, e.last.z};
  e.handle = e.type == EntityType::Ped ? n::CreateChar(n::kPedTypeCivMale, e.model, at) : n::CreateCar(e.model, at);
  n::MarkModelAsNoLongerNeeded(e.model);
  if (!e.handle) return false;
  if (e.type == EntityType::Vehicle) n::SetVehicleQuaternion(e.handle, {e.last.qx, e.last.qy, e.last.qz, e.last.qw});
  else n::SetCharHeading(e.handle, e.last.heading());
  if (e.sim_owner == my_id)
    make_live(e);
  else
    make_follower(e);
  log::info("Entity {}: local copy spawned (handle {}), {}", e.id, e.handle, e.live ? "live" : "follower");
  return true;
}

void EntitySync::make_live(Entity& e) {
  e.live = true;
  // Wake up from the last synced pose, keeping its motion.
  const EntityState& s = e.last;
  if (e.type == EntityType::Ped) {
    n::FreezeCharPosition(e.handle, false);
    n::SetCharCollision(e.handle, true);
    n::SetCharCoordinatesNoOffset(e.handle, {s.x, s.y, s.z});
    n::SetCharHeading(e.handle, s.heading());
    // Real health again (followers sit on a buffer). A dead entity stays dead.
    n::SetCharMaxHealth(e.handle, std::max<std::uint32_t>(200, s.health));
    if (!n::IsCharDead(e.handle)) n::SetCharHealth(e.handle, s.health ? s.health : 1);
    n::TaskWanderStandard(e.handle); // M4 test behaviour; M6 replays the host script's tasks
  } else {
    n::FreezeCarPosition(e.handle, false);
    n::SetCarCollision(e.handle, true);
    n::SetCarCoordinatesNoOffset(e.handle, {s.x, s.y, s.z});
    n::SetVehicleQuaternion(e.handle, {s.qx, s.qy, s.qz, s.qw});
    n::SetCarForwardSpeed(e.handle, std::hypot(s.vx, s.vy));
    n::SetCarHealth(e.handle, s.health);
  }
}

void EntitySync::make_follower(Entity& e) {
  e.live = false;
  if (e.type == EntityType::Ped) {
    n::ClearCharTasks(e.handle);
    n::FreezeCharPosition(e.handle, true);
    n::SetCharCollision(e.handle, false);
    if (!n::IsCharDead(e.handle)) {
      n::SetCharMaxHealth(e.handle, kPedBuffer);
      n::SetCharHealth(e.handle, kPedBuffer);
    }
  } else {
    n::FreezeCarPosition(e.handle, true);
    n::SetCarCollision(e.handle, false);
    n::SetCarHealth(e.handle, kCarBuffer);
  }
}

EntityState EntitySync::read_local(const Entity& e, std::uint32_t now_ms) {
  EntityState s;
  s.id = e.id;
  s.time_ms = now_ms;
  if (e.type == EntityType::Ped) {
    const n::Vector3 p = n::GetCharCoordinates(e.handle), v = n::GetCharVelocity(e.handle);
    s.x = p.x, s.y = p.y, s.z = p.z, s.vx = v.x, s.vy = v.y, s.vz = v.z;
    EntityState::set_heading(s, n::GetCharHeading(e.handle));
    // Health 0 is the "dead" signal followers act on.
    s.health = n::IsCharDead(e.handle)
                   ? 0
                   : static_cast<std::uint16_t>(std::clamp<std::uint32_t>(n::GetCharHealth(e.handle), 1, 0xFFFF));
  } else {
    const n::Vector3 p = n::GetCarCoordinates(e.handle), v = n::GetCarSpeedVector(e.handle);
    const n::Vector4 q = n::GetVehicleQuaternion(e.handle);
    s.x = p.x, s.y = p.y, s.z = p.z, s.vx = v.x, s.vy = v.y, s.vz = v.z;
    s.qx = q.x, s.qy = q.y, s.qz = q.z, s.qw = q.w;
    s.health = static_cast<std::uint16_t>(std::min<std::uint32_t>(n::GetCarHealth(e.handle), 0xFFFF));
  }
  return s;
}

void EntitySync::pose_follower(Entity& e, const EntityState& s) {
  if (e.type == EntityType::Ped) {
    if (s.health == 0) { // the owner's copy died: die here too, once, and stop posing it
      if (!e.shown_dead) {
        n::FreezeCharPosition(e.handle, false);
        n::SetCharHealth(e.handle, 0);
        e.shown_dead = true;
      }
      return;
    }
    n::SetCharCoordinatesNoOffset(e.handle, {s.x, s.y, s.z});
    n::SetCharHeading(e.handle, s.heading());
  } else {
    n::SetCarCoordinatesNoOffset(e.handle, {s.x, s.y, s.z});
    n::SetVehicleQuaternion(e.handle, {s.qx, s.qy, s.qz, s.qw});
  }
}

void EntitySync::destroy_copy(Entity& e) {
  if (exists(e.type, e.handle)) {
    if (e.type == EntityType::Ped)
      n::DeleteChar(e.handle);
    else
      n::DeleteCar(e.handle);
  }
  e.handle = 0;
}

// ---------------------------------------------------------------- per frame

void EntitySync::tick(std::uint32_t now_ms, std::uint8_t my_id, bool hosting, const std::vector<PlayerPos>& players,
                      net::Session& net) {
  if (hosting && now_ms - last_eval_ms_ >= kEvalIntervalMs) {
    last_eval_ms_ = now_ms;
    evaluate_ownership(players, my_id, net);
  }

  const std::uint32_t interval = 1000 / std::max<std::uint32_t>(send_hz, 1);
  for (auto& [id, e] : ents_) {
    if (!ensure_spawned(e, my_id)) continue;
    if (e.sim_owner == my_id) {
      if (!e.live) make_live(e);
      e.last = read_local(e, now_ms);
      if (now_ms - e.last_send_ms >= interval) {
        e.last_send_ms = now_ms;
        e.last.seq = ++e.seq;
        net.send_game(encode_entity_state(e.last), false);
      }
    } else {
      if (e.live) make_follower(e);
      if (auto s = e.interp.sample(now_ms)) pose_follower(e, *s);
    }
  }
}

void EntitySync::clear(bool was_host) {
  for (auto& [id, e] : ents_) {
    if (was_host) {
      if (exists(e.type, e.handle) && !e.live) make_live(e); // back to an ordinary local entity
    } else {
      destroy_copy(e);
    }
  }
  ents_.clear();
}

std::vector<Damage> EntitySync::collect_hits(n::Ped attacker) {
  std::vector<Damage> hits;
  if (!attacker) return hits;
  for (auto& [id, e] : ents_) {
    if (e.live || e.shown_dead || !exists(e.type, e.handle)) continue;
    std::uint32_t lost = 0;
    bool by_attacker = false;
    if (e.type == EntityType::Ped) {
      const std::uint32_t h = n::GetCharHealth(e.handle);
      lost = h < kPedBuffer ? kPedBuffer - h : 0;
      by_attacker = n::HasCharBeenDamagedByChar(e.handle, attacker);
      if (lost) n::SetCharHealth(e.handle, kPedBuffer);
      n::ClearCharLastDamageEntity(e.handle);
    } else {
      const std::uint32_t h = n::GetCarHealth(e.handle);
      lost = h < kCarBuffer ? kCarBuffer - h : 0;
      by_attacker = n::HasCarBeenDamagedByChar(e.handle, attacker);
      if (lost) n::SetCarHealth(e.handle, kCarBuffer);
      n::ClearCarLastDamageEntity(e.handle);
    }
    if (lost && by_attacker)
      hits.push_back({DamageTarget::Entity, id, static_cast<std::uint16_t>(std::min<std::uint32_t>(lost, 0xFFFF)), 0});
  }
  return hits;
}

void EntitySync::apply_damage(std::uint32_t id, std::uint16_t amount, std::uint8_t my_id) {
  auto it = ents_.find(id);
  if (it == ents_.end()) return;
  Entity& e = it->second;
  if (e.sim_owner != my_id || !exists(e.type, e.handle)) return;
  if (e.type == EntityType::Ped) {
    if (n::IsCharDead(e.handle)) return;
    const std::uint32_t h = n::GetCharHealth(e.handle);
    n::SetCharHealth(e.handle, h > amount ? h - amount : 0);
  } else {
    const std::uint32_t h = n::GetCarHealth(e.handle);
    n::SetCarHealth(e.handle, h > amount ? h - amount : 0);
  }
  log::info("Entity {}: took {} damage (owner-applied)", id, amount);
}

std::vector<EntityStatus> EntitySync::status() const {
  std::vector<EntityStatus> out;
  for (const auto& [id, e] : ents_)
    out.push_back({id, e.type, e.sim_owner, e.handle != 0, e.live, e.last.x, e.last.y, e.last.z, e.migrations});
  return out;
}

} // namespace ivc::sync
