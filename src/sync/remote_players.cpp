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

void handle_hotkeys() {
  const net::Mode mode = session().status().mode;
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
        break;
      case K::Left:
        log::info("Player {} '{}' left", e.id, e.text);
        drop_remote(e.id);
        g_vehicles.drop_owner(e.id);
        break;
      case K::Connected:
        log::info("Connected; we are player {}", e.id);
        break;
      case K::Disconnected:
        log::info("Disconnected: {}", e.text);
        drop_all();
        g_vehicles.clear();
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

  for (const auto& u : net.drain_states()) remote(u.id).interp.push(u.state, u.arrival_ms);
  for (const auto& u : net.drain_vehicles()) g_vehicles.push_remote(u);
  g_vehicles.update_remote(now, me); // stand-in cars first, so puppets can be seated in them

  std::vector<RemoteStatus> remotes;
  for (auto& [id, r] : g_remotes) {
    float dist = 0;
    if (auto smp = r.interp.sample(now)) { // once per frame: sampling advances the timeline
      const n::Vector3 at{smp->x, smp->y, smp->z};
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
}

Status status() {
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

} // namespace ivc::coop
