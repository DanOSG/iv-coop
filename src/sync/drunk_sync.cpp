#include "sync/drunk_sync.h"

#include <windows.h>

#include <cmath>

#include "core/log.h"
#include "game/native_hooks.h"
#include "net/bytes.h"

namespace ivc::drunk_sync {
namespace {
namespace n = natives;
using native_hooks::Call;

constexpr std::uint32_t kSetPedIsDrunk = 0x67CC007C, kSetDrunkCam = 0x74B90C48, kGetRootCam = 0x75E005F1;
constexpr DWORD kResendMs = 4000, kReapplyMs = 2000;

struct State {
  bool drunk = false;
  float cam = 0;           // SET_DRUNK_CAM intensity (0: off)
  std::uint32_t cam_ms = 0; // its ramp time
  std::uint8_t mask = 0;   // player ids who take part
  bool operator==(const State&) const = default;
};

// Host: what the scripts did (written by the hooks, sent by tick).
State g_host;
bool g_start_pending = false; // the host just became drunk: work out who is present
bool g_dirty = false;
std::uint8_t g_seq = 0;
DWORD g_last_send = 0;

// Joiner: the host's latest state, and what we applied from it.
State g_in;
std::uint8_t g_in_seq = 0;
bool g_in_valid = false;
bool g_me_drunk = false;    // we set our ped drunk
float g_cam_applied = 0;
std::uint32_t g_cam_ms_applied = 0;
n::Ped g_puppet_drunk = 0;  // host copy we made drunk
DWORD g_puppet_at = 0;

n::Ped player_ped() {
  const n::Ped p = n::GetPlayerChar(n::GetPlayerId());
  return p && n::DoesCharExist(p) ? p : 0;
}

void set_cam(float intensity, std::uint32_t ms) {
  n::Camera cam = 0;
  n::invoke(kGetRootCam, &cam);
  if (cam) n::invoke(kSetDrunkCam, cam, intensity, ms);
}

// ---- host

void on_drunk(const Call& c) {
  const auto ped = c.arg<n::Ped>(0);
  const bool on = c.arg<std::uint32_t>(1) != 0;
  const n::Ped me = player_ped();
  if (!me || ped != me) return; // other peds (a date) are not synced yet
  if (on && !g_host.drunk) g_start_pending = true;
  if (g_host.drunk != on) g_dirty = true;
  g_host.drunk = on;
  if (!on) g_host.mask = 0;
}

void on_cam(const Call& c) {
  const float v = c.arg<float>(1);
  const auto ms = c.arg<std::uint32_t>(2);
  if (v == g_host.cam && ms == g_host.cam_ms) return;
  g_host.cam = v, g_host.cam_ms = ms;
  g_dirty = true;
}

void send_state(net::Session& net) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(++g_seq).put(static_cast<std::uint8_t>(g_host.drunk ? 1 : 0))
      .put(g_host.mask).put(g_host.cam).put(g_host.cam_ms);
  net.send_game(std::move(w.data()), true);
}

void host_tick(const Frame& f, net::Session& net, DWORD now) {
  if (g_start_pending) {
    g_start_pending = false;
    std::uint8_t mask = 0;
    if (const n::Ped me = player_ped()) {
      const n::Vector3 a = n::GetCharCoordinates(me);
      for (const Mate& m : f.mates) {
        if (!m.ped || !n::DoesCharExist(m.ped) || m.id >= 8) continue;
        const n::Vector3 b = n::GetCharCoordinates(m.ped);
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) <= kPresentRange) mask |= std::uint8_t(1u << m.id);
      }
    }
    g_host.mask = mask;
    log::info("Drunk: host is drunk, present partners mask {:#x}", mask);
  }
  if (g_dirty || (g_host.drunk && now - g_last_send >= kResendMs)) {
    g_dirty = false, g_last_send = now;
    send_state(net);
  }
}

// ---- joiner

void release_me() {
  if (g_me_drunk) {
    if (const n::Ped me = player_ped()) n::invoke(kSetPedIsDrunk, me, false);
    g_me_drunk = false;
  }
  if (g_cam_applied != 0 || g_cam_ms_applied != 0) {
    set_cam(0.0f, 0);
    g_cam_applied = 0, g_cam_ms_applied = 0;
  }
}

void release_puppet() {
  if (g_puppet_drunk && n::DoesCharExist(g_puppet_drunk)) n::invoke(kSetPedIsDrunk, g_puppet_drunk, false);
  g_puppet_drunk = 0;
}

void client_tick(const Frame& f, DWORD now) {
  const bool want_me = g_in_valid && g_in.drunk && (g_in.mask & (1u << f.my_id));
  const n::Ped me = player_ped();
  if (want_me && me) {
    if (!g_me_drunk) {
      n::invoke(kSetPedIsDrunk, me, true);
      g_me_drunk = true;
      log::info("Drunk: made drunk with the host");
    }
    if (g_in.cam != g_cam_applied || g_in.cam_ms != g_cam_ms_applied) {
      set_cam(g_in.cam, g_in.cam_ms);
      g_cam_applied = g_in.cam, g_cam_ms_applied = g_in.cam_ms;
    }
  } else if (!want_me) {
    release_me();
  }

  // The host's copy walks drunk for everyone while the host is.
  const bool want_puppet = g_in_valid && g_in.drunk && f.host_puppet && n::DoesCharExist(f.host_puppet);
  if (want_puppet) {
    if (g_puppet_drunk != f.host_puppet || now - g_puppet_at >= kReapplyMs) {
      g_puppet_drunk = f.host_puppet, g_puppet_at = now;
      n::invoke(kSetPedIsDrunk, f.host_puppet, true);
    }
  } else if (g_puppet_drunk) {
    release_puppet();
  }
}

} // namespace

void register_hooks() {
  native_hooks::on(kSetPedIsDrunk, "SET_PED_IS_DRUNK", on_drunk);
  native_hooks::on(kSetDrunkCam, "SET_DRUNK_CAM", on_cam);
}

void tick(const Frame& f, net::Session& net) {
  if (!f.in_session) {
    release_me();
    release_puppet();
    g_in_valid = false, g_in = {}, g_host = {}, g_start_pending = g_dirty = false;
    return;
  }
  const DWORD now = GetTickCount();
  if (f.hosting) host_tick(f, net, now);
  else client_tick(f, now);
}

void on_message(const net::GameMessage& m) {
  if (m.from != net::kHostId) return;
  net::Reader r(m.data);
  std::uint8_t type, sender, seq, drunk;
  State s;
  if (!r.get(type) || !r.get(sender) || !r.get(seq) || !r.get(drunk) || !r.get(s.mask) || !r.get(s.cam) ||
      !r.get(s.cam_ms))
    return;
  if (g_in_valid && static_cast<std::int8_t>(seq - g_in_seq) <= 0) return; // old or repeated
  s.drunk = drunk != 0;
  g_in = s, g_in_seq = seq, g_in_valid = true;
}

} // namespace ivc::drunk_sync
