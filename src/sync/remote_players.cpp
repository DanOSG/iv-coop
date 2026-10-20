#include "sync/remote_players.h"

#include <cmath>
#include <map>
#include <memory>
#include <mutex>

#include "core/input.h"
#include "core/log.h"
#include "sync/local_player.h"
#include "sync/player_state.h"
#include "sync/puppet.h"
#include "sync/entity_sync.h"
#include "sync/vehicle_sync.h"

namespace ivc::coop {
namespace {
namespace n = natives;

struct Remote {
  std::string name;
  sync::Interpolator interp;
  sync::Puppet puppet;
};

Settings g_cfg;
std::unique_ptr<net::Session> g_session; // created on first use, not inside DllMain
std::map<std::uint8_t, Remote> g_remotes;
sync::VehicleSync g_vehicles;
sync::EntitySync g_entities;
bool g_was_host = false;
bool g_host_was_down = true, g_join_was_down = true;
std::uint32_t g_last_send = 0;
std::uint16_t g_seq = 0;

std::mutex g_status_mutex;
Status g_status;

net::Session& session() {
  if (!g_session) g_session = std::make_unique<net::Session>();
  return *g_session;
}

Remote& remote(std::uint8_t id) {
  auto [it, inserted] = g_remotes.try_emplace(id);
  if (inserted) {
    it->second.interp.delay_ms = g_cfg.interp_delay_ms;
    it->second.puppet = sync::Puppet(g_cfg.puppet_model, g_cfg.animated_puppets);
  }
  return it->second;
}

void drop_remote(std::uint8_t id) {
  if (auto it = g_remotes.find(id); it != g_remotes.end()) {
    it->second.puppet.despawn();
    g_remotes.erase(it);
  }
}

void drop_all() {
  for (auto& [id, r] : g_remotes) r.puppet.despawn();
  g_remotes.clear();
}

// M4 test bed (host only): spawn networked entities in front of the player.
struct PendingSpawn {
  sync::EntityType type;
  std::uint32_t model;
};
std::optional<PendingSpawn> g_pending_spawn;
bool g_spawn_ped_down = true, g_spawn_car_down = true, g_delete_down = true;

void handle_entity_keys(bool hosting) {
  if (!hosting) return;
  if (input::pressed_ctrl_shift('P', g_spawn_ped_down))
    g_pending_spawn = PendingSpawn{sync::EntityType::Ped, n::GetHashKey("F_Y_MULTIPLAYER")};
  if (input::pressed_ctrl_shift('V', g_spawn_car_down))
    g_pending_spawn = PendingSpawn{sync::EntityType::Vehicle, n::GetHashKey("TAXI")};
  if (input::pressed_ctrl_shift('X', g_delete_down)) g_entities.host_delete_all(session());
}

void process_pending_spawn(n::Ped me) {
  if (!g_pending_spawn || !me) return;
  const auto [type, model] = *g_pending_spawn;
  n::RequestModel(model);
  if (!n::HasModelLoaded(model)) return;
  const n::Vector3 at = n::GetOffsetFromCharInWorldCoords(me, {0.0f, type == sync::EntityType::Ped ? 4.0f : 7.0f, 0.0f});
  n::Handle h = type == sync::EntityType::Ped ? n::CreateChar(n::kPedTypeCivMale, model, at) : n::CreateCar(model, at);
  n::MarkModelAsNoLongerNeeded(model);
  g_pending_spawn.reset();
  if (!h) {
    log::error("Test entity spawn failed");
    return;
  }
  if (type == sync::EntityType::Ped) n::TaskWanderStandard(h);
  g_entities.host_register(h, type, model, session());
}

void handle_hotkeys() {
  const net::Mode mode = session().status().mode;
  handle_entity_keys(mode == net::Mode::Hosting);
  if (input::pressed(g_cfg.host_key, g_host_was_down)) {
    if (mode == net::Mode::Idle) {
      if (session().host(g_cfg.port, g_cfg.name, g_cfg.bind_address))
        log::info("Hosting on UDP {} as '{}'", g_cfg.port, g_cfg.name);
      else
        log::error("Hosting failed: {}", session().status().last_error);
    } else {
      log::info("Leaving session ({})", net::to_string(mode));
      session().leave();
    }
  }
  if (input::pressed(g_cfg.join_key, g_join_was_down)) {
    if (mode == net::Mode::Idle) {
      if (session().join(g_cfg.join_address, g_cfg.port, g_cfg.name))
        log::info("Joining {}:{} as '{}'", g_cfg.join_address, g_cfg.port, g_cfg.name);
      else
        log::error("Join failed: {}", session().status().last_error);
    } else {
      log::info("Leaving session ({})", net::to_string(mode));
      session().leave();
    }
  }
}

} // namespace

void configure(const Settings& s) {
  g_cfg = s;
  g_vehicles.interp_delay_ms = s.interp_delay_ms;
  g_vehicles.physics_follow = s.physics_follow;
  g_vehicles.force_type = s.vehicle_force_type;
}

void tick() {
  handle_hotkeys();
  net::Session& net = session();

  for (const auto& e : net.drain_events()) {
    using K = net::SessionEvent::Kind;
    switch (e.kind) {
      case K::Joined:
        log::info("Player {} '{}' joined", e.id, e.text);
        remote(e.id).name = e.text;
        if (net.status().mode == net::Mode::Hosting) g_entities.on_player_joined(e.id, net);
        break;
      case K::Left:
        log::info("Player {} '{}' left", e.id, e.text);
        drop_remote(e.id);
        g_vehicles.drop_owner(e.id);
        g_entities.on_player_left(e.id);
        break;
      case K::Connected:
        log::info("Connected; we are player {}", e.id);
        break;
      case K::Disconnected:
        log::info("Disconnected: {}", e.text);
        drop_all();
        g_vehicles.clear();
        g_entities.clear(g_was_host);
        break;
    }
  }

  const std::uint32_t now = net::steady_ms();
  const net::SessionStatus ss = net.status();
  const bool active = ss.mode == net::Mode::Hosting || ss.mode == net::Mode::Joined;
  auto local = sync::capture_local(now);
  const n::Ped me = local ? n::GetPlayerChar(n::GetPlayerId()) : 0;

  // Which vehicle/seat we're in, and the state of the vehicle if we're its sim owner.
  std::optional<sync::VehicleState> owned;
  if (local) {
    const auto puppet_of = [](n::Ped ped) -> std::optional<std::uint8_t> {
      for (const auto& [id, r] : g_remotes)
        if (r.puppet.ped() == ped) return id;
      return std::nullopt;
    };
    owned = g_vehicles.capture_local(me, ss.my_id, *local, now, puppet_of);
  }

  // Fixed cadence: advance the send clock by whole intervals so uneven frame times don't
  // drag the rate below send_hz; after a long stall (pause), resync instead of bursting.
  const std::uint32_t interval = 1000 / g_cfg.send_hz;
  if (active && local && now - g_last_send >= interval) {
    g_last_send = (now - g_last_send >= 4 * interval) ? now : g_last_send + interval;
    local->seq = ++g_seq;
    net.send_state(*local);
    if (owned) net.send_vehicle(*owned);
  }

  // Only players currently in the session count. Packets from a player who just left can
  // still be queued behind their "left" event; accepting them would resurrect a frozen
  // ghost of that player (and keep entities it owned from being reassigned).
  const auto in_session = [&ss](std::uint8_t id) {
    for (const auto& p : ss.players)
      if (p.id == id) return true;
    return false;
  };
  for (auto it = g_remotes.begin(); it != g_remotes.end();) {
    if (!in_session(it->first)) {
      it->second.puppet.despawn();
      it = g_remotes.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& u : net.drain_states())
    if (in_session(u.id)) remote(u.id).interp.push(u.state, u.arrival_ms);
  for (const auto& u : net.drain_vehicles())
    if (in_session(u.id)) g_vehicles.push_remote(u);
  g_vehicles.update_remote(now, me); // stand-in cars first, so puppets can be seated in them

  // Everyone's position, for entity sim-ownership decisions (host).
  std::vector<sync::PlayerPos> positions;
  if (local) positions.push_back({ss.my_id, {local->x, local->y, local->z}});

  std::vector<RemoteStatus> remotes;
  for (auto& [id, r] : g_remotes) {
    float dist = 0;
    if (auto smp = r.interp.sample(now)) { // once per frame: sampling advances the timeline
      const n::Vector3 at{smp->x, smp->y, smp->z};
      positions.push_back({id, at});
      if (r.puppet.ensure(at)) {
        const n::Vehicle veh = g_vehicles.handle_for(smp->vehicle_id);
        if (veh) {
          r.puppet.sit_in(veh, smp->seat);
        } else {
          // On foot, or their car hasn't arrived here yet: follow their position on foot.
          if (r.puppet.in_vehicle()) r.puppet.leave_vehicle(at);
          r.puppet.drive(*smp, now);
        }
      }
      if (local) dist = std::hypot(smp->x - local->x, smp->y - local->y, smp->z - local->z);
    }
    const auto& st = r.interp.stats();
    const unsigned frames = st.interpolated + st.extrapolated;
    std::uint32_t rtt = 0;
    for (const auto& p : ss.players)
      if (p.id == id) rtt = p.rtt_ms;
    remotes.push_back({id, r.name, rtt, r.puppet.spawned(),
                       frames ? 100.0f * static_cast<float>(st.extrapolated) / static_cast<float>(frames) : 0.0f,
                       dist, r.puppet.phase(), r.puppet.error_m(), r.puppet.snaps()});
  }

  // Networked entities (M4).
  if (active) {
    g_was_host = ss.mode == net::Mode::Hosting;
    process_pending_spawn(me);
    for (const auto& m : net.drain_game())
      if (in_session(m.from)) g_entities.on_message(m, ss.my_id);
    g_entities.tick(now, ss.my_id, g_was_host, positions, net);
  }
  const auto entities = g_entities.status();
  static std::uint32_t last_elog = now;
  if (!entities.empty() && now - last_elog >= 10000) {
    last_elog = now;
    for (const auto& e : entities)
      log::info("Entity {} ({}): sim owner #{}{}, at {:.1f} {:.1f} {:.1f}, {} migrations", e.id,
                e.type == sync::EntityType::Ped ? "ped" : "vehicle", e.sim_owner, e.live ? " (live here)" : "", e.x,
                e.y, e.z, e.migrations);
  }

  static std::uint32_t last_log = now;
  if (!remotes.empty() && now - last_log >= 10000) {
    last_log = now;
    for (const auto& r : remotes)
      log::info("Remote #{} '{}': {} err {:.1f} m, {} snaps, {:.1f} m away, extrap {:.1f}%", r.id, r.name, r.action,
                r.error_m, r.snaps, r.distance_m, r.extrapolated_pct);
  }

  const auto vehicles = g_vehicles.status();
  static std::uint32_t last_vlog = now;
  if (!vehicles.empty() && now - last_vlog >= 10000) {
    last_vlog = now;
    for (const auto& v : vehicles)
      log::info("Remote vehicle 0x{:08X} (owner #{}): {} err {:.1f} m, {} snaps, gain {:.3f}, steer {:+.2f}{}",
                v.vehicle_id, v.owner, v.spawned ? "spawned" : "loading", v.error_m, v.snaps, v.gain, v.steer,
                v.blocked ? ", BLOCKED" : "");
  }

  std::lock_guard lock(g_status_mutex);
  g_status.session = ss;
  g_status.remotes = std::move(remotes);
  g_status.vehicles = vehicles;
  g_status.entities = entities;
}

Status status() {
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

} // namespace ivc::coop
