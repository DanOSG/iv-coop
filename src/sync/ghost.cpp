#include "sync/ghost.h"

#include <windows.h>

#include <chrono>
#include <mutex>

#include "core/log.h"
#include "game/natives.h"
#include "net/transport.h"
#include "sync/player_state.h"

namespace ivc::ghost {
namespace {
namespace n = natives;

Settings g_cfg;
net::LoopbackTransport g_net;
sync::Interpolator g_interp;

bool g_enabled = false;
bool g_key_was_down = true;
n::Ped g_ped = 0;
n::Blip g_blip = 0;
std::uint32_t g_model = 0;
std::uint32_t g_last_send = 0;
std::uint16_t g_seq = 0;

std::mutex g_status_mutex;
Status g_status;

std::uint32_t now_ms() {
  using namespace std::chrono;
  return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void despawn() {
  if (g_blip) n::RemoveBlip(g_blip), g_blip = 0;
  if (g_ped && n::DoesCharExist(g_ped)) n::DeleteChar(g_ped);
  g_ped = 0;
  g_interp.clear();
}

void set_phase(std::string p) {
  std::lock_guard lock(g_status_mutex);
  if (g_status.phase != p) log::info("Ghost: {}", p);
  g_status.phase = std::move(p);
}

// Returns true once the ghost exists.
bool ensure_spawned(const n::Vector3& at) {
  if (g_ped && n::DoesCharExist(g_ped)) return true;
  if (g_ped) {
    log::warn("Ghost ped {} vanished; respawning", g_ped);
    g_ped = 0;
  }
  if (!g_model) g_model = n::GetHashKey(g_cfg.model.c_str());
  n::RequestModel(g_model);
  if (!n::HasModelLoaded(g_model)) {
    set_phase("loading model " + g_cfg.model);
    return false;
  }
  g_ped = n::CreateChar(n::kPedTypeCivMale, g_model, at);
  n::MarkModelAsNoLongerNeeded(g_model);
  if (!g_ped) {
    set_phase("CREATE_CHAR failed");
    return false;
  }
  n::SetBlockingOfNonTemporaryEvents(g_ped, true);
  n::SetCharInvincible(g_ped, true);
  n::SetCharCollision(g_ped, false);
  n::FreezeCharPosition(g_ped, true);
  g_blip = n::AddBlipForChar(g_ped);
  log::info("Ghost spawned: ped {} model 0x{:08X}", g_ped, g_model);
  return true;
}

void poll_toggle() {
  const bool down = (GetAsyncKeyState(g_cfg.toggle_key) & 0x8000) != 0;
  if (down && !g_key_was_down) {
    g_enabled = !g_enabled;
    log::info("Ghost {}", g_enabled ? "enabled" : "disabled");
    if (!g_enabled) {
      despawn();
      set_phase("off");
    }
  }
  g_key_was_down = down;
}

} // namespace

void configure(const Settings& s) {
  g_cfg = s;
  g_net.latency_ms = s.latency_ms;
  g_net.jitter_ms = s.jitter_ms;
  g_net.loss = s.loss;
  g_interp.delay_ms = s.interp_delay_ms;
  set_phase("off");
}

void tick() {
  poll_toggle();
  const std::uint32_t now = now_ms();
  g_net.set_clock(now);

  const n::Player player = n::GetPlayerId();
  const n::Ped me = n::GetPlayerChar(player);
  if (!me || !n::DoesCharExist(me)) {
    if (g_enabled) set_phase("no player ped yet");
    return;
  }

  // --- capture + send (what a remote client will do) ---
  sync::PlayerState s;
  const n::Vector3 pos = n::GetCharCoordinates(me);
  const n::Vector3 vel = n::GetCharVelocity(me);
  s.time_ms = now;
  s.x = pos.x, s.y = pos.y, s.z = pos.z;
  s.vx = vel.x, s.vy = vel.y, s.vz = vel.z;
  s.heading = n::GetCharHeading(me);
  s.model = n::GetCharModel(me);
  s.in_vehicle = n::IsCharInAnyCar(me);

  Status st;
  {
    std::lock_guard lock(g_status_mutex);
    g_status.player_x = s.x, g_status.player_y = s.y, g_status.player_z = s.z, g_status.player_heading = s.heading;
    st = g_status;
  }

  if (!g_enabled) {
    std::lock_guard lock(g_status_mutex);
    g_status.enabled = false;
    g_status.spawned = false;
    return;
  }

  if (now - g_last_send >= 1000 / g_cfg.send_hz) {
    g_last_send = now;
    s.seq = ++g_seq;
    const auto bytes = sync::encode(s);
    g_net.send(0, net::Channel::Unreliable, bytes);
    ++st.sent;
  }

  // --- receive + apply (what the host does for each remote player) ---
  g_net.poll(now, [&](int, std::span<const std::uint8_t> payload) {
    if (auto ps = sync::decode_player_state(payload)) {
      g_interp.push(*ps);
      ++st.received;
    }
  });

  // The loopback shares our clock, so the sender's "now" is just now.
  if (auto smp = g_interp.sample(now)) {
    const n::Vector3 at{smp->x + g_cfg.offset_x, smp->y, smp->z};
    if (ensure_spawned(at)) {
      n::SetCharCoordinatesNoOffset(g_ped, at);
      n::SetCharHeading(g_ped, smp->heading);
      set_phase("following");
    }
  } else {
    set_phase("waiting for snapshots");
  }

  std::lock_guard lock(g_status_mutex);
  g_status.enabled = true;
  g_status.spawned = g_ped != 0;
  g_status.sent = st.sent;
  g_status.received = st.received;
  g_status.buffered = static_cast<std::uint32_t>(g_interp.buffered());
  g_status.dropped = g_interp.dropped();
}

Status status() {
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

} // namespace ivc::ghost
