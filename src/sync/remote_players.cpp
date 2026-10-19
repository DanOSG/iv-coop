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
    it->second.puppet = sync::Puppet(g_cfg.puppet_model);
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

void configure(const Settings& s) { g_cfg = s; }

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
        break;
      case K::Connected:
        log::info("Connected; we are player {}", e.id);
        break;
      case K::Disconnected:
        log::info("Disconnected: {}", e.text);
        drop_all();
        break;
    }
  }

  const std::uint32_t now = net::steady_ms();
  const net::SessionStatus ss = net.status();
  const bool active = ss.mode == net::Mode::Hosting || ss.mode == net::Mode::Joined;
  auto local = sync::capture_local(now);

  if (active && local && now - g_last_send >= 1000 / g_cfg.send_hz) {
    g_last_send = now;
    local->seq = ++g_seq;
    net.send_state(*local);
  }

  for (const auto& u : net.drain_states()) remote(u.id).interp.push(u.state, u.arrival_ms);

  std::vector<RemoteStatus> remotes;
  for (auto& [id, r] : g_remotes) {
    float dist = 0;
    if (auto smp = r.interp.sample(now)) { // once per frame: sampling advances the timeline
      const n::Vector3 at{smp->x, smp->y, smp->z};
      if (r.puppet.ensure(at)) r.puppet.place(at, smp->heading);
      if (local) dist = std::hypot(smp->x - local->x, smp->y - local->y, smp->z - local->z);
    }
    const auto& st = r.interp.stats();
    const unsigned frames = st.interpolated + st.extrapolated;
    std::uint32_t rtt = 0;
    for (const auto& p : ss.players)
      if (p.id == id) rtt = p.rtt_ms;
    remotes.push_back({id, r.name, rtt, r.puppet.spawned(),
                       frames ? 100.0f * static_cast<float>(st.extrapolated) / static_cast<float>(frames) : 0.0f,
                       dist});
  }

  std::lock_guard lock(g_status_mutex);
  g_status.session = ss;
  g_status.remotes = std::move(remotes);
}

Status status() {
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

} // namespace ivc::coop
