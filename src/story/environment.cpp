#include "story/environment.h"

#include <windows.h>

#include <cstdlib>

#include "core/log.h"
#include "game/natives.h"
#include "net/bytes.h"
#include "story/story_protocol.h"
#include "sync/session_settings.h"

namespace ivc::story::environment {
namespace {
namespace n = natives;
using n::invoke;

constexpr std::uint32_t kGetTimeOfDay = 0x384B3876, kSetTimeOfDay = 0x52100540, kGetCurrentWeather = 0x27E421EA,
                        kForceWeather = 0x7EFB5077, kForceWeatherNow = 0x63737D31, kReleaseWeather = 0x3A115D9D;
constexpr DWORD kSendMs = 2000;
constexpr int kMaxClockErrorMin = 1;

struct State {
  std::uint8_t hour = 0, minute = 0;
  std::uint32_t weather = 0;
};

State read_local() {
  int h = 0, m = 0;
  std::uint32_t w = 0;
  invoke(kGetTimeOfDay, &h, &m);
  invoke(kGetCurrentWeather, &w);
  return {static_cast<std::uint8_t>(h), static_cast<std::uint8_t>(m), w};
}

// Host.
DWORD g_last_send = 0;

// Joiner.
bool g_have = false, g_forced = false, g_pending = false;
State g_target;
std::uint32_t g_forced_weather = ~0u;

} // namespace

void host_tick(bool hosting, net::Session& net) {
  if (!hosting) return;
  const DWORD now = GetTickCount();
  if (now - g_last_send < kSendMs) return;
  g_last_send = now;
  const State s = read_local();
  net::Writer w;
  w.put(StoryMsg::Environment).put(std::uint8_t{0}).put(s.hour).put(s.minute).put(s.weather);
  net.send_game(std::move(w.data()), false);
}

void client_apply(std::span<const std::uint8_t> msg) {
  net::Reader r(msg);
  StoryMsg type;
  std::uint8_t sender;
  State s;
  if (!r.get(type) || !r.get(sender) || !r.get(s.hour) || !r.get(s.minute) || !r.get(s.weather)) return;
  if (s.hour > 23 || s.minute > 59) return;
  g_target = s, g_have = true, g_pending = true;
}

void client_tick() {
  if (!session_settings::current().world_sync) {
    if (g_forced) clear(); // back to our own weather
    return;
  }
  if (!g_pending) return;
  g_pending = false;
  const State local = read_local();
  const int want = g_target.hour * 60 + g_target.minute, have = local.hour * 60 + local.minute;
  int diff = std::abs(want - have);
  diff = diff > 720 ? 1440 - diff : diff;
  if (diff > kMaxClockErrorMin) invoke(kSetTimeOfDay, static_cast<int>(g_target.hour), static_cast<int>(g_target.minute));
  if (g_target.weather != g_forced_weather) {
    invoke(g_forced ? kForceWeather : kForceWeatherNow, g_target.weather);
    log::info("Environment: weather {} -> {} ({}), clock {:02}:{:02}", g_forced_weather == ~0u ? -1 : static_cast<int>(g_forced_weather),
              g_target.weather, g_forced ? "blend" : "snap", g_target.hour, g_target.minute);
    g_forced_weather = g_target.weather, g_forced = true;
  }
}

void clear() {
  if (g_forced) invoke(kReleaseWeather);
  g_have = g_forced = g_pending = false;
  g_forced_weather = ~0u;
}

} // namespace ivc::story::environment
