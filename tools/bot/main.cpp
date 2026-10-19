// iv-coop-bot: a headless session client for testing hosting without a second copy of the
// game. It joins a host, then walks a circle around the host's player (following them as
// they move), sending PlayerState exactly like the in-game client does.
//
//   iv-coop-bot [--host 127.0.0.1] [--port 41300] [--name Bot] [--radius 4] [--speed 1.6]
//               [--phase 0] [--skew 123456] [--fixed] [--serve] [--seconds N] [--retry]
//
// Run up to three at once (different --name/--phase) to fill a 4-player session.
// --serve makes the bot the host instead (circling the origin), for testing the session
// layer with no game running at all. --seconds quits after N seconds.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>

#include "net/session.h"

namespace {

std::atomic<bool> g_quit{false};

BOOL WINAPI on_ctrl(DWORD) {
  g_quit = true;
  return TRUE;
}

struct Args {
  std::string host = "127.0.0.1";
  std::uint16_t port = 41300;
  std::string name = "Bot";
  float radius = 4.0f;
  float speed = 1.6f;     // m/s, a brisk walk
  float phase = 0.0f;     // degrees around the circle to start at
  std::int32_t skew = 123456; // added to our clock, so the receiver must learn the offset
  bool follow = true;     // circle the host's current position (vs. where we first saw them)
  bool serve = false;     // host the session ourselves
  int seconds = 0;        // 0 = run until Ctrl+C
  bool retry = false;     // keep re-joining until admitted (host not up yet, session full, ...)
};

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (k == "--host") a.host = next();
    else if (k == "--port") a.port = static_cast<std::uint16_t>(std::atoi(next()));
    else if (k == "--name") a.name = next();
    else if (k == "--radius") a.radius = static_cast<float>(std::atof(next()));
    else if (k == "--speed") a.speed = static_cast<float>(std::atof(next()));
    else if (k == "--phase") a.phase = static_cast<float>(std::atof(next()));
    else if (k == "--skew") a.skew = std::atoi(next());
    else if (k == "--fixed") a.follow = false;
    else if (k == "--serve") a.serve = true;
    else if (k == "--seconds") a.seconds = std::atoi(next());
    else if (k == "--retry") a.retry = true;
    else std::fprintf(stderr, "unknown argument %s\n", k.c_str());
  }
  if (a.radius < 0.5f) a.radius = 0.5f;
  return a;
}

} // namespace

int main(int argc, char** argv) {
  using namespace ivc;
  const Args a = parse(argc, argv);
  SetConsoleCtrlHandler(on_ctrl, TRUE);
  std::setvbuf(stdout, nullptr, _IONBF, 0); // output may be redirected to a file; don't buffer it

  net::Session session;
  if (a.serve ? !session.host(a.port, a.name, "127.0.0.1") : !session.join(a.host, a.port, a.name)) {
    std::fprintf(stderr, "%s failed: %s\n", a.serve ? "host" : "join", session.status().last_error.c_str());
    return 1;
  }
  if (a.serve)
    std::printf("[%s] hosting on 127.0.0.1:%u\n", a.name.c_str(), a.port);
  else
    std::printf("[%s] joining %s:%u ...\n", a.name.c_str(), a.host.c_str(), a.port);

  constexpr float kPi = 3.14159265f;
  float angle = a.phase * kPi / 180.0f;
  bool have_center = a.serve; // a serving bot circles the origin
  const std::uint32_t started = net::steady_ms();
  float cx = 0, cy = 0, cz = 0;
  std::uint16_t seq = 0;
  std::uint32_t last_send = 0, last_report = net::steady_ms();
  std::map<std::uint8_t, unsigned> received;
  auto prev = std::chrono::steady_clock::now();
  bool was_joined = false;

  while (!g_quit) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    const auto t = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(t - prev).count();
    prev = t;
    const std::uint32_t now = net::steady_ms();
    if (a.seconds > 0 && now - started >= static_cast<std::uint32_t>(a.seconds) * 1000) break;

    for (const auto& e : session.drain_events()) {
      using K = net::SessionEvent::Kind;
      if (e.kind == K::Connected) std::printf("[%s] connected as player #%u\n", a.name.c_str(), e.id);
      if (e.kind == K::Joined) std::printf("[%s] player #%u '%s' is here\n", a.name.c_str(), e.id, e.text.c_str());
      if (e.kind == K::Left) std::printf("[%s] player #%u '%s' left\n", a.name.c_str(), e.id, e.text.c_str());
      if (e.kind == K::Disconnected) {
        std::printf("[%s] disconnected: %s\n", a.name.c_str(), e.text.c_str());
        if (a.retry && !a.serve) {
          std::this_thread::sleep_for(std::chrono::seconds(3));
          std::printf("[%s] retrying...\n", a.name.c_str());
          have_center = false;
          if (!session.join(a.host, a.port, a.name)) g_quit = true;
        } else {
          g_quit = true;
        }
      }
    }
    for (const auto& u : session.drain_states()) {
      ++received[u.id];
      if (u.id == net::kHostId && (a.follow || !have_center)) {
        cx = u.state.x, cy = u.state.y, cz = u.state.z;
        if (!have_center) std::printf("[%s] host is at %.1f %.1f %.1f, circling\n", a.name.c_str(), cx, cy, cz);
        have_center = true;
      }
    }

    const net::SessionStatus st = session.status();
    const bool active = st.mode == net::Mode::Joined || st.mode == net::Mode::Hosting;
    if (active) was_joined = true;
    if (!active || !have_center) continue;

    // Walk the circle at constant speed; heading follows the direction of travel.
    angle += a.speed / a.radius * dt;
    sync::PlayerState s;
    s.x = cx + std::cos(angle) * a.radius;
    s.y = cy + std::sin(angle) * a.radius;
    s.z = cz;
    s.vx = -std::sin(angle) * a.speed;
    s.vy = std::cos(angle) * a.speed;
    s.vz = 0;
    // IV heading: degrees counter-clockwise from north (+Y).
    float h = std::atan2(-s.vx, s.vy) * 180.0f / kPi;
    s.heading = h < 0 ? h + 360.0f : h;

    if (now - last_send >= 33) {
      last_send = now;
      s.seq = ++seq;
      s.time_ms = now + static_cast<std::uint32_t>(a.skew);
      session.send_state(s);
    }

    if (now - last_report >= 5000) {
      last_report = now;
      std::printf("[%s] #%u  players %zu  sent %u  recv:", a.name.c_str(), st.my_id, st.players.size(),
                  st.states_sent);
      for (const auto& [id, n] : received) std::printf(" #%u=%u", id, n);
      for (const auto& p : st.players)
        if (p.id != st.my_id && (a.serve || p.id == net::kHostId)) std::printf("  rtt#%u %u ms", p.id, p.rtt_ms);
      std::printf("\n");
    }
  }

  session.leave();
  std::printf("[%s] bye\n", a.name.c_str());
  return was_joined ? 0 : 2;
}
