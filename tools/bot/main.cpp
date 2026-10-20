// iv-coop-bot: a headless session client for testing hosting without a second copy of the
// game. It joins a host, then walks a circle around the host's player (following them as
// they move), sending PlayerState exactly like the in-game client does.
//
//   iv-coop-bot [--host 127.0.0.1] [--port 41300] [--name Bot] [--radius 4] [--speed 1.6]
//               [--phase 0] [--skew 123456] [--fixed] [--serve] [--seconds N] [--retry]
//               [--drive MODEL] [--ride [--seat N]]
//
// --drive sultan: circle the host in that car (sends VehicleState as the car's owner).
// --ride: sit in the host's car (passenger seat --seat, default 0) whenever they drive one.
//
// Run up to three at once (different --name/--phase) to fill a 4-player session.
// --serve makes the bot the host instead (circling the origin), for testing the session
// layer with no game running at all. --seconds quits after N seconds.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <format>
#include <optional>
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
  int pause_every = 0;    // stand still for 3 s every N seconds (0 = never)
  std::string drive;      // vehicle model name: circle in this car instead of on foot
  bool ride = false;      // sit in the host's vehicle whenever they're in one
  int seat = 0;           // passenger seat for --ride
};

// Model name -> hash, as the game's GET_HASH_KEY does (Jenkins one-at-a-time, lowercase).
std::uint32_t model_hash(const std::string& name) {
  std::uint32_t h = 0;
  for (unsigned char c : name) {
    h += static_cast<unsigned char>(std::tolower(c));
    h += h << 10;
    h ^= h >> 6;
  }
  h += h << 3;
  h ^= h >> 11;
  h += h << 15;
  return h;
}

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
    else if (k == "--pause-every") a.pause_every = std::atoi(next());
    else if (k == "--drive") a.drive = next();
    else if (k == "--ride") a.ride = true;
    else if (k == "--seat") a.seat = std::atoi(next());
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
  std::uint32_t last_send = 0, last_report = net::steady_ms(), last_report_cost = 0;
  std::map<std::uint8_t, unsigned> received;
  // Same interpolation the game runs per remote player, so its health can be checked
  // from the bot's report with no game involved.
  std::map<std::uint8_t, sync::Interpolator> interps;
  struct Gaps {
    std::uint32_t last_arrival = 0, last_stamp = 0, max_arrival_gap = 0, max_stamp_gap = 0;
  };
  std::map<std::uint8_t, Gaps> gaps; // per sender, reset each report
  std::map<std::uint32_t, unsigned> vehicles_received;
  std::uint32_t host_vehicle = sync::kNoVehicle;
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
      interps[u.id].push(u.state, u.arrival_ms);
      auto& g = gaps[u.id];
      if (g.last_arrival) g.max_arrival_gap = std::max(g.max_arrival_gap, u.arrival_ms - g.last_arrival);
      if (g.last_stamp) g.max_stamp_gap = std::max(g.max_stamp_gap, u.state.time_ms - g.last_stamp);
      g.last_arrival = u.arrival_ms, g.last_stamp = u.state.time_ms;
      if (u.id == net::kHostId && (a.follow || !have_center)) {
        cx = u.state.x, cy = u.state.y, cz = u.state.z;
        if (!have_center) std::printf("[%s] host is at %.1f %.1f %.1f, circling\n", a.name.c_str(), cx, cy, cz);
        have_center = true;
      }
      if (u.id == net::kHostId) {
        if (u.state.vehicle_id != host_vehicle)
          std::printf("[%s] host vehicle is now 0x%08X\n", a.name.c_str(), u.state.vehicle_id);
        host_vehicle = u.state.vehicle_id;
      }
    }
    for (const auto& u : session.drain_vehicles()) ++vehicles_received[u.state.vehicle_id];

    for (auto& [id, ip] : interps) ip.sample(now);

    const net::SessionStatus st = session.status();
    const bool active = st.mode == net::Mode::Joined || st.mode == net::Mode::Hosting;
    if (active) was_joined = true;
    if (!active || !have_center) continue;

    // Walk the circle at constant speed; heading follows the direction of travel.
    const bool paused = a.pause_every > 0 && ((now - started) / 1000) % static_cast<std::uint32_t>(a.pause_every) < 3;
    const float speed = paused ? 0.0f : a.speed;
    angle += speed / a.radius * dt;
    sync::PlayerState s;
    s.x = cx + std::cos(angle) * a.radius;
    s.y = cy + std::sin(angle) * a.radius;
    s.z = cz;
    s.vx = -std::sin(angle) * speed;
    s.vy = std::cos(angle) * speed;
    s.vz = 0;
    // IV heading: degrees counter-clockwise from north (+Y). Tangent to the circle, so a
    // paused bot keeps facing the way it was walking.
    float h = std::atan2(std::sin(angle), std::cos(angle)) * 180.0f / kPi;
    s.heading = h < 0 ? h + 360.0f : h;

    std::optional<sync::VehicleState> car;
    if (a.ride && host_vehicle != sync::kNoVehicle) {
      // Passenger in the host's car: our position is theirs; the host seats our puppet.
      s.x = cx, s.y = cy, s.z = cz, s.vx = s.vy = s.vz = 0;
      s.in_vehicle = 1, s.vehicle_id = host_vehicle, s.seat = static_cast<std::int8_t>(a.seat);
    } else if (!a.drive.empty()) {
      s.in_vehicle = 1, s.vehicle_id = (static_cast<std::uint32_t>(st.my_id) << 24) | 1, s.seat = -1;
      sync::VehicleState v;
      v.vehicle_id = s.vehicle_id;
      v.model = model_hash(a.drive);
      // The host's z is their ped's centre, ~1 m above the road; put the car on the road.
      v.x = s.x, v.y = s.y, v.z = s.z - 1.0f;
      const float hr = s.heading * kPi / 180.0f; // yaw about +Z
      v.qx = 0, v.qy = 0, v.qz = std::sin(hr / 2), v.qw = std::cos(hr / 2);
      v.vx = s.vx, v.vy = s.vy, v.vz = 0;
      v.speed = speed;
      v.colour[0] = 0, v.colour[1] = 0; // black
      car = v;
    }

    if (now - last_send >= 33) {
      last_send = now;
      s.seq = ++seq;
      s.time_ms = now + static_cast<std::uint32_t>(a.skew);
      session.send_state(s);
      if (car) {
        car->seq = s.seq, car->time_ms = s.time_ms;
        session.send_vehicle(*car);
      }
    }

    if (now - last_report >= 5000) {
      last_report = now;
      // Build the whole line first and write it once: many small writes to a redirected
      // stdout each block on the reader and can stall this loop (and so our sends).
      std::string line = std::format("[{}] #{}  players {}  sent {}  report took {} ms  recv:", a.name, st.my_id,
                                     st.players.size(), st.states_sent, last_report_cost);
      for (const auto& [id, n] : received) {
        const auto& is = interps[id].stats();
        const unsigned frames = is.interpolated + is.extrapolated;
        auto& g = gaps[id];
        line += std::format(" #{}={} (extrap {:.1f}%, offset {}, max gap arrive {}/stamp {} ms)", id, n,
                            frames ? 100.0 * is.extrapolated / frames : 0.0, is.offset_ms, g.max_arrival_gap,
                            g.max_stamp_gap);
        g.max_arrival_gap = g.max_stamp_gap = 0;
      }
      for (const auto& p : st.players)
        if (p.id != st.my_id && (a.serve || p.id == net::kHostId)) line += std::format("  rtt#{} {} ms", p.id, p.rtt_ms);
      for (const auto& [vid, n] : vehicles_received) line += std::format("  veh 0x{:08X}={}", vid, n);
      line += '\n';
      std::fwrite(line.data(), 1, line.size(), stdout);
      last_report_cost = net::steady_ms() - now;
    }
  }

  session.leave();
  std::printf("[%s] bye\n", a.name.c_str());
  return was_joined ? 0 : 2;
}
