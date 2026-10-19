#include "sync/ghost.h"

#include <mutex>

#include "core/input.h"
#include "core/log.h"
#include "net/protocol.h"
#include "net/session.h"
#include "net/transport.h"
#include "sync/local_player.h"
#include "sync/player_state.h"
#include "sync/puppet.h"

namespace ivc::ghost {
namespace {
namespace n = natives;

Settings g_cfg;
net::LoopbackTransport g_net;
sync::Interpolator g_interp;
sync::Puppet g_puppet;

bool g_enabled = false;
bool g_key_was_down = true;
std::uint32_t g_last_send = 0;
std::uint16_t g_seq = 0;
std::uint32_t g_sent = 0, g_received = 0;

std::mutex g_status_mutex;
Status g_status;

void set_phase(std::string p) {
  std::lock_guard lock(g_status_mutex);
  if (g_status.phase != p) log::info("Ghost: {}", p);
  g_status.phase = std::move(p);
}

void disable() {
  g_puppet.despawn();
  g_interp.clear();
  g_sent = g_received = 0;
  set_phase("off");
}

} // namespace

void configure(const Settings& s) {
  g_cfg = s;
  g_puppet = sync::Puppet(s.model, s.animated);
  g_net.latency_ms = s.latency_ms;
  g_net.jitter_ms = s.jitter_ms;
  g_net.loss = s.loss;
  g_interp.delay_ms = s.interp_delay_ms;
  set_phase("off");
}

void tick() {
  if (input::pressed(g_cfg.toggle_key, g_key_was_down)) {
    g_enabled = !g_enabled;
    log::info("Ghost {}", g_enabled ? "enabled" : "disabled");
    if (!g_enabled) disable();
  }

  const std::uint32_t now = net::steady_ms();
  g_net.set_clock(now);
  auto local = sync::capture_local(now);
  if (local) {
    std::lock_guard lock(g_status_mutex);
    g_status.player_x = local->x, g_status.player_y = local->y, g_status.player_z = local->z;
    g_status.player_heading = local->heading;
  }
  if (!g_enabled) {
    std::lock_guard lock(g_status_mutex);
    g_status.enabled = g_status.spawned = false;
    return;
  }
  if (!local) {
    set_phase("no player ped yet");
    return;
  }

  // --- capture + send (what a remote client does) ---
  if (now - g_last_send >= 1000 / g_cfg.send_hz) {
    g_last_send = now;
    local->seq = ++g_seq;
    g_net.send(0, net::Channel::Unreliable, net::encode_state(0, *local));
    ++g_sent;
  }

  // --- receive + apply (what the host does for each remote player) ---
  g_net.poll(now, [&](int, std::span<const std::uint8_t> payload) {
    net::Reader r(payload);
    net::MsgType type{};
    std::uint8_t id;
    sync::PlayerState s;
    if (r.get(type) && type == net::MsgType::PlayerState && net::decode_state(r, id, s)) {
      g_interp.push(s, now);
      ++g_received;
    }
  });

  // Uses only our local clock: the interpolator learns the sender's timeline from arrivals,
  // exactly as it must over a real network where the clocks differ.
  if (auto smp = g_interp.sample(now)) {
    smp->x += g_cfg.offset_x;
    if (g_puppet.ensure({smp->x, smp->y, smp->z})) g_puppet.drive(*smp, now);
    set_phase(g_puppet.phase());
  } else {
    set_phase("waiting for snapshots");
  }

  static std::uint32_t last_stats_log = now;
  const auto& is = g_interp.stats();
  if (now - last_stats_log >= 10000) {
    last_stats_log = now;
    log::info("Ghost stats: interpolated {} / extrapolated {} frames, offset {} ms, stale {}, recv {}/{} sent",
              is.interpolated, is.extrapolated, is.offset_ms, is.stale, g_received, g_sent);
  }

  std::lock_guard lock(g_status_mutex);
  g_status.enabled = true;
  g_status.spawned = g_puppet.spawned();
  g_status.sent = g_sent;
  g_status.received = g_received;
  g_status.buffered = static_cast<std::uint32_t>(g_interp.buffered());
  g_status.stale = is.stale;
  g_status.interpolated = is.interpolated;
  g_status.extrapolated = is.extrapolated;
  g_status.offset_ms = is.offset_ms;
}

Status status() {
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

} // namespace ivc::ghost
