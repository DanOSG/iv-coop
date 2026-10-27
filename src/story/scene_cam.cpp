#include "story/scene_cam.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "core/log.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "net/bytes.h"
#include "story/cutscene.h"
#include "story/story_sync.h"

namespace ivc::story::scene_cam {
namespace {
namespace n = natives;
using n::invoke;
using native_hooks::Call;

constexpr std::uint32_t kSetCamActive = 0x43E42686, kActivateScripted = 0x3EBE11B9, kDestroyCam = 0x14334EEE,
                        kCreateCam = 0x694A0DC1, kSetCamPos = 0x152F6314, kSetCamRot = 0x746744D1,
                        kSetCamFov = 0x55D470C2, kGetCamFov = 0x7BF4652D, kSetPlayerControl = 0x1A6203EA,
                        kSetPlayerControlAdv = 0x31E25160, kFadeOut = 0x65DE621C, kFadeIn = 0x4D72200,
                        kFadeOutUnhacked = 0x42D250A7, kFadeInUnhacked = 0x5F9218C3;
constexpr DWORD kSendMs = 40;            // 25 Hz
constexpr DWORD kTimeoutMs = 1500;       // joiner: no samples this long = the scene is over
constexpr DWORD kFadeFailsafeMs = 15000; // joiner: a fade-out of ours with no fade-in after the scene ended
constexpr float kSnapMeters = 25.0f, kSnapDegrees = 100.0f, kSmoothSeconds = 0.045f;
enum Op : std::uint8_t { OpCam = 0, OpEnd = 1, OpFade = 2 };

struct Pose {
  float p[3]{}, r[3]{}, fov = 45.0f;
};

// ---------------------------------------------------------------- host
bool g_hosting = false;
std::vector<n::Camera> g_cams; // cameras the story scripts made active, last = current
bool g_scripted = false;       // ACTIVATE_SCRIPTED_CAMS is on
bool g_control_off = false;    // the host's player control is off
bool g_sent_scene = false;
DWORD g_last_send = 0;
struct PendingFade {
  std::uint8_t kind;
  std::uint32_t ms;
  bool scene;
};
std::vector<PendingFade> g_fades;

bool story_call(const Call& c) { return g_hosting && is_story_script(c.script); }
bool scene_on() { return g_scripted && !g_cams.empty(); }

void drop_cam(n::Camera cam) { std::erase(g_cams, cam); }

void captured_fade(const Call& c, std::uint8_t kind) {
  if (story_call(c)) g_fades.push_back({kind, c.arg<std::uint32_t>(0), scene_on() || g_control_off});
}

void captured_control(const Call& c) {
  if (story_call(c) && c.arg<std::int32_t>(0) == n::GetPlayerId()) g_control_off = !c.arg<bool>(1);
}

// ---------------------------------------------------------------- joiner
bool g_scene = false; // the host's scene is on (samples keep coming)
bool g_have_sample = false;
Pose g_target, g_cur;
DWORD g_last_sample = 0;
std::chrono::steady_clock::time_point g_last_frame;
n::Camera g_cam = 0;
bool g_cam_live = false;      // our script camera exists and is active
bool g_control_taken = false; // we switched the player's control off
bool g_faded_out = false;     // we faded the screen out for the host
DWORD g_fade_since = 0;
struct Fade {
  std::uint8_t kind;
  std::uint32_t ms;
  bool scene;
};
std::vector<Fade> g_pending_fades;

float wrap_delta(float d) {
  while (d > 180.0f) d -= 360.0f;
  while (d < -180.0f) d += 360.0f;
  return d;
}

void teardown() {
  if (g_cam_live) {
    invoke(kActivateScripted, false, false);
    invoke(kSetCamActive, g_cam, false);
    invoke(kDestroyCam, g_cam);
    g_cam = 0, g_cam_live = false;
    log::info("Scene: camera released");
  }
  if (g_control_taken) {
    invoke(kSetPlayerControl, n::GetPlayerId(), true);
    g_control_taken = false;
  }
}

void fade_in() {
  invoke(kFadeIn, std::uint32_t{500});
  g_faded_out = false;
}

void begin_scene() {
  invoke(kCreateCam, 14, &g_cam);
  if (!g_cam) return;
  invoke(kSetCamActive, g_cam, true);
  invoke(kActivateScripted, true, true);
  g_cam_live = true;
  // Partners keep control of their character through the scene: everyone sees the host's shot, and
  // anyone but the host can walk around in it (only the host's scripts freeze the host).
  log::info("Scene: watching the host's camera");
}

} // namespace

void register_hooks() {
  using native_hooks::before;
  using native_hooks::on;
  on(kSetCamActive, "SET_CAM_ACTIVE", [](const Call& c) {
    if (!story_call(c)) return;
    const auto cam = c.arg<n::Camera>(0);
    drop_cam(cam);
    if (c.arg<bool>(1)) g_cams.push_back(cam);
  });
  on(kActivateScripted, "ACTIVATE_SCRIPTED_CAMS", [](const Call& c) {
    if (story_call(c)) g_scripted = c.arg<bool>(0);
  });
  before(kDestroyCam, "DESTROY_CAM", [](const Call& c) {
    if (story_call(c)) drop_cam(c.arg<n::Camera>(0));
  });
  on(kSetPlayerControl, "SET_PLAYER_CONTROL", captured_control);
  on(kSetPlayerControlAdv, "SET_PLAYER_CONTROL_ADVANCED", captured_control);
  on(kFadeOut, "DO_SCREEN_FADE_OUT", [](const Call& c) { captured_fade(c, 0); });
  on(kFadeIn, "DO_SCREEN_FADE_IN", [](const Call& c) { captured_fade(c, 1); });
  on(kFadeOutUnhacked, "DO_SCREEN_FADE_OUT_UNHACKED", [](const Call& c) { captured_fade(c, 2); });
  on(kFadeInUnhacked, "DO_SCREEN_FADE_IN_UNHACKED", [](const Call& c) { captured_fade(c, 3); });
}

void host_tick(bool hosting, net::Session& net) {
  g_hosting = hosting;
  if (!hosting) {
    g_cams.clear(), g_fades.clear();
    g_scripted = g_control_off = g_sent_scene = false;
    return;
  }
  for (const PendingFade& f : g_fades) {
    net::Writer w;
    w.put(kMsg).put(std::uint8_t{0}).put(static_cast<std::uint8_t>(OpFade)).put(f.kind).put(f.ms).put(
        std::uint8_t{f.scene});
    net.send_game(std::move(w.data()), true);
  }
  g_fades.clear();

  const DWORD now = GetTickCount();
  if (!scene_on()) {
    if (g_sent_scene) {
      g_sent_scene = false;
      net::Writer w;
      w.put(kMsg).put(std::uint8_t{0}).put(static_cast<std::uint8_t>(OpEnd));
      net.send_game(std::move(w.data()), true);
      log::info("Scene: host's scene ended");
    }
    return;
  }
  if (now - g_last_send < kSendMs) return;
  g_last_send = now;
  const n::Camera cam = g_cams.back();
  const n::Vector3 p = n::GetCamPos(cam), r = n::GetCamRot(cam);
  float fov = 45.0f;
  invoke(kGetCamFov, cam, &fov);
  if (!std::isfinite(p.x + p.y + p.z + r.x + r.y + r.z + fov)) return;
  if (!g_sent_scene) log::info("Scene: host's scene began (camera {})", cam);
  g_sent_scene = true;
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(static_cast<std::uint8_t>(OpCam)).put(p.x).put(p.y).put(p.z).put(r.x).put(
      r.y).put(r.z).put(fov);
  net.send_game(std::move(w.data()), false);
}

void client_apply(std::span<const std::uint8_t> msg) {
  net::Reader r(msg);
  std::uint8_t type, sender, op;
  if (!r.get(type) || !r.get(sender) || !r.get(op)) return;
  if (op == OpCam) {
    Pose s;
    if (!r.get(s.p[0]) || !r.get(s.p[1]) || !r.get(s.p[2]) || !r.get(s.r[0]) || !r.get(s.r[1]) || !r.get(s.r[2]) ||
        !r.get(s.fov))
      return;
    g_target = s;
    g_last_sample = GetTickCount();
    if (!g_scene) g_have_sample = false; // a new scene: snap to its first sample
    g_scene = true;
  } else if (op == OpEnd) {
    g_scene = false;
    g_fade_since = GetTickCount();
  } else if (op == OpFade) {
    std::uint8_t kind, scene;
    std::uint32_t ms;
    if (r.get(kind) && r.get(ms) && r.get(scene) && kind <= 3) g_pending_fades.push_back({kind, ms, scene != 0});
  }
}

void client_tick(bool watch) {
  const DWORD now = GetTickCount();
  const bool cutscene_on = cutscene::playing();
  if (g_scene && now - g_last_sample > kTimeoutMs) g_scene = false, g_fade_since = now;
  if (!watch || cutscene_on) { // our own setting, or a cutscene is playing here: stand down
    g_pending_fades.clear();
    if (g_cam_live || g_control_taken) teardown();
    if (g_faded_out) fade_in();
    return;
  }

  for (const Fade& f : g_pending_fades) {
    const bool out = f.kind == 0 || f.kind == 2;
    if (out && !f.scene) continue; // fades unrelated to a scene aren't ours to mirror
    static constexpr std::uint32_t kHashes[] = {kFadeOut, kFadeIn, kFadeOutUnhacked, kFadeInUnhacked};
    invoke(kHashes[f.kind], f.ms);
    g_faded_out = out;
    if (out) g_fade_since = now;
  }
  g_pending_fades.clear();
  if (g_faded_out && !g_scene && now - g_fade_since > kFadeFailsafeMs) fade_in();

  if (!g_scene) {
    if (g_cam_live || g_control_taken) teardown();
    return;
  }
  if (!g_cam_live) {
    begin_scene();
    if (!g_cam_live) return;
    g_have_sample = false;
  }

  // Ease the camera toward the latest sample; snap on cuts (a new shot) and at the start.
  const auto t = std::chrono::steady_clock::now();
  const float dt = std::chrono::duration<float>(t - g_last_frame).count();
  g_last_frame = t;
  if (!g_have_sample) {
    g_cur = g_target;
    g_have_sample = true;
  } else {
    const float dx = g_target.p[0] - g_cur.p[0], dy = g_target.p[1] - g_cur.p[1], dz = g_target.p[2] - g_cur.p[2];
    bool snap = dx * dx + dy * dy + dz * dz > kSnapMeters * kSnapMeters;
    for (int i = 0; i < 3; ++i) snap = snap || std::fabs(wrap_delta(g_target.r[i] - g_cur.r[i])) > kSnapDegrees;
    if (snap) {
      g_cur = g_target;
    } else {
      const float k = 1.0f - std::exp(-std::clamp(dt, 0.0f, 0.25f) / kSmoothSeconds);
      for (int i = 0; i < 3; ++i) {
        g_cur.p[i] += (g_target.p[i] - g_cur.p[i]) * k;
        g_cur.r[i] += wrap_delta(g_target.r[i] - g_cur.r[i]) * k;
      }
      g_cur.fov += (g_target.fov - g_cur.fov) * k;
    }
  }
  invoke(kSetCamPos, g_cam, g_cur.p[0], g_cur.p[1], g_cur.p[2]);
  invoke(kSetCamRot, g_cam, g_cur.r[0], g_cur.r[1], g_cur.r[2]);
  invoke(kSetCamFov, g_cam, g_cur.fov);
}

void clear() {
  g_cams.clear(), g_fades.clear(), g_pending_fades.clear();
  g_scripted = g_control_off = g_sent_scene = false;
  g_scene = false;
  teardown();
  if (g_faded_out) fade_in();
}

} // namespace ivc::story::scene_cam
