// iv-coop-bot: a headless session client for testing hosting without a second copy of the
// game. It joins a host, then walks a circle around the host's player (following them as
// they move), sending PlayerState exactly like the in-game client does.
//
//   iv-coop-bot [--host 127.0.0.1] [--port 41300] [--name Bot] [--radius 4] [--speed 1.6]
//               [--phase 0] [--skew 123456] [--fixed] [--serve] [--seconds N] [--retry]
//               [--drive MODEL] [--ride [--seat N]] [--replay FILE.ivcrec]
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
#include <cstring>
#include <format>
#include <optional>
#include <map>
#include <string>
#include <thread>

#include "net/session.h"
#include "story/story_protocol.h"
#include "sync/entity_protocol.h"

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
  bool test_entity = false; // --serve only: create entity 999, then hand it to player #1
  int leave_after = 0;      // leave the session N seconds after joining (0 = never)
  bool armed = false;       // hold a pistol (weapon type 7) and aim at the host
  bool downed = false;      // report being downed (stand still) until a partner revives us
  int shoot_host = 0;       // fire at the host every N seconds (10 damage per hit); implies armed
  int health = 200;         // starting (and respawn) health
  std::string replay;       // --serve: replay a recorded host session (.ivcrec) to the first client
  std::string model;        // appear as this model (e.g. PLAYER = Niko); empty = receivers' fallback
};

struct Record {
  std::uint32_t t_ms;
  bool reliable;
  std::vector<std::uint8_t> data;
};

std::vector<Record> load_recording(const std::string& path) {
  std::vector<Record> out;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  char magic[4];
  std::uint32_t version = 0;
  if (std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "IVCR", 4) == 0 && std::fread(&version, 4, 1, f) == 1) {
    for (;;) {
      Record r;
      std::uint8_t rel;
      std::uint32_t len;
      if (std::fread(&r.t_ms, 4, 1, f) != 1 || std::fread(&rel, 1, 1, f) != 1 || std::fread(&len, 4, 1, f) != 1) break;
      r.reliable = rel != 0;
      r.data.resize(len);
      if (std::fread(r.data.data(), 1, len, f) != len) break;
      out.push_back(std::move(r));
    }
  }
  std::fclose(f);
  return out;
}

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
    else if (k == "--test-entity") a.test_entity = true;
    else if (k == "--leave-after") a.leave_after = std::atoi(next());
    else if (k == "--armed") a.armed = true;
    else if (k == "--downed") a.downed = true;
    else if (k == "--health") a.health = std::atoi(next());
    else if (k == "--replay") a.replay = next(), a.serve = true;
    else if (k == "--model") a.model = next();
    else if (k == "--shoot-host") a.shoot_host = std::atoi(next()), a.armed = true;
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
  bool up = a.serve ? session.host(a.port, a.name, "127.0.0.1") : session.join(a.host, a.port, a.name);
  // --retry when serving: wait for the port (e.g. the game is still hosting on it).
  while (!up && a.serve && a.retry && !g_quit) {
    std::printf("[%s] host failed (%s); retrying in 2 s\n", a.name.c_str(), session.status().last_error.c_str());
    std::this_thread::sleep_for(std::chrono::seconds(2));
    up = session.host(a.port, a.name, "127.0.0.1");
  }
  if (!up) {
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

  // Networked entities (M4). When the host makes us an entity's sim owner we "simulate"
  // it: a ped walks a small circle around where we got it, a car holds still.
  struct BotEntity {
    sync::EntityType type{};
    std::uint8_t owner = 0;
    sync::EntityState last{};
    float cx = 0, cy = 0, cz = 0, angle = 0;
    std::uint16_t seq = 0;
  };
  std::map<std::uint32_t, BotEntity> entities;
  std::uint32_t last_entity_send = 0;
  unsigned entity_states_received = 0;

  // Replay of a recorded host session (test rig for client-side story playback).
  // Loaded when the first client joins, so the bot can be started before the recording
  // has been finished and saved.
  std::vector<Record> replay;
  std::size_t replay_next = 0;
  std::uint32_t replay_start = 0; // set when the first client joins
  bool replaying_states = false;  // the recording carries the host's own states: don't circle

  // Combat (M5): our health, death/respawn, and shooting at the host.
  constexpr std::uint32_t kPistol = 7;
  int health = a.health;
  std::uint32_t dead_until = 0; // respawn time while dead
  std::uint16_t shots = 0;
  std::uint32_t last_shot_ms = 0;
  unsigned damage_taken = 0, damage_dealt = 0;
  auto prev = std::chrono::steady_clock::now();
  bool was_joined = false;
  std::uint32_t joined_at = 0;
  bool downed = a.downed; // --downed, until a partner revives us

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
      if (e.kind == K::Joined) {
        std::printf("[%s] player #%u '%s' is here\n", a.name.c_str(), e.id, e.text.c_str());
        if (!a.replay.empty() && !replay_start) {
          replay = load_recording(a.replay);
          std::printf("[%s] loaded %zu recorded messages (%.1f s) from %s\n", a.name.c_str(), replay.size(),
                      replay.empty() ? 0.0 : replay.back().t_ms / 1000.0, a.replay.c_str());
          if (!replay.empty()) {
            replay_start = now + 3000; // give the client a moment to settle
            std::printf("[%s] replay starts in 3 s\n", a.name.c_str());
          }
        }
      }
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
      if (u.id == net::kHostId && ((a.follow && !downed) || !have_center)) {
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

    const std::uint8_t my_id = session.status().my_id;
    static unsigned world_ops = 0;
    for (const auto& gm : session.drain_game()) {
      // downed::Msg::Revive (41) aimed at us: back up.
      if (gm.data.size() >= 3 && gm.data[0] == 41 && gm.data[2] == session.status().my_id && downed) {
        downed = false;
        std::printf("[%s] revived by player #%u%c", a.name.c_str(), gm.from, 10);
        continue;
      }
      if (story::is_story_msg(gm.data)) { // print what the host's story sends us
        if (const auto d = story::decode(gm.data)) {
          switch (d->type) {
            case story::StoryMsg::TextBlock:
              std::printf("[%s] story: text block slot %u -> '%s'\n", a.name.c_str(), d->text_block.slot,
                          story::to_string(d->text_block.name).c_str());
              break;
            case story::StoryMsg::BlipAdd:
              std::printf("[%s] story: blip %u added (kind %u, entity %u, %.1f %.1f)\n", a.name.c_str(),
                          d->blip_add.id, static_cast<unsigned>(d->blip_add.kind), d->blip_add.entity, d->blip_add.x,
                          d->blip_add.y);
              break;
            case story::StoryMsg::BlipProp:
              std::printf("[%s] story: blip %u prop %u = %d\n", a.name.c_str(), d->blip_prop.id,
                          static_cast<unsigned>(d->blip_prop.prop), d->blip_prop.ivalue);
              break;
            case story::StoryMsg::BlipRemove:
              std::printf("[%s] story: blip %u removed\n", a.name.c_str(), d->blip_id);
              break;
            case story::StoryMsg::World:
              ++world_ops;
              if (world_ops == 1 || world_ops % 25 == 0)
                std::printf("[%s] story: world op #%u (kind %u)\n", a.name.c_str(), world_ops,
                            gm.data.size() > 2 ? gm.data[2] : 0u);
              break;
            case story::StoryMsg::Payout:
              std::printf("[%s] story: payout $%d\n", a.name.c_str(), d->payout);
              break;
            case story::StoryMsg::Text:
              std::printf("[%s] story: text '%s'\n", a.name.c_str(), story::to_string(d->text.key).c_str());
              break;
          }
        }
        continue;
      }
      const auto m = sync::decode_entity_msg(gm.data);
      if (!m) continue;
      if (m->type == sync::GameMsg::EntityCreate) {
        auto& e = entities[m->create.id];
        e.type = m->create.type;
        e.owner = m->create.sim_owner;
        e.last.id = m->create.id, e.last.x = m->create.x, e.last.y = m->create.y, e.last.z = m->create.z;
        e.last.qx = m->create.qx, e.last.qy = m->create.qy, e.last.qz = m->create.qz, e.last.qw = m->create.qw;
        std::printf("[%s] entity %u created (owner #%u)\n", a.name.c_str(), m->create.id, e.owner);
      } else if (m->type == sync::GameMsg::EntityDelete) {
        entities.erase(m->id);
        std::printf("[%s] entity %u deleted\n", a.name.c_str(), m->id);
      } else if (m->type == sync::GameMsg::EntityOwner) {
        if (auto it = entities.find(m->id); it != entities.end()) {
          auto& e = it->second;
          e.owner = m->owner;
          if (e.owner == my_id) {
            e.cx = e.last.x, e.cy = e.last.y, e.cz = e.last.z, e.angle = 0;
            std::printf("[%s] entity %u: now simulated by us\n", a.name.c_str(), m->id);
          } else {
            std::printf("[%s] entity %u: sim owner -> #%u\n", a.name.c_str(), m->id, e.owner);
          }
        }
      } else if (m->type == sync::GameMsg::Damage) {
        const auto& d = m->damage;
        if (d.kind == sync::DamageTarget::Player && d.target == my_id && !dead_until) {
          health -= d.amount;
          damage_taken += d.amount;
          std::printf("[%s] hit by #%u for %u (weapon %u), health %d\n", a.name.c_str(), gm.from, d.amount, d.weapon,
                      std::max(health, 0));
          if (health <= 0) {
            dead_until = now + 5000;
            std::printf("[%s] died; respawning in 5 s\n", a.name.c_str());
          }
        }
      } else if (m->type == sync::GameMsg::EntityState) {
        if (auto it = entities.find(m->state.id); it != entities.end() && gm.from == it->second.owner) {
          it->second.last = m->state;
          ++entity_states_received;
        }
      }
    }

    // Replay: send recorded messages on their original schedule.
    if (replay_start && static_cast<std::int32_t>(now - replay_start) >= 0 && replay_next < replay.size()) {
      const std::uint32_t replay_t = now - replay_start;
      while (replay_next < replay.size() && replay[replay_next].t_ms <= replay_t) {
        const auto& rec = replay[replay_next];
        // The recorded host's own movement and car: become them (see Session::send_state).
        if (!rec.data.empty() && rec.data[0] == net::kRecPlayerState &&
            rec.data.size() == 1 + sizeof(sync::PlayerState)) {
          sync::PlayerState ps;
          std::memcpy(&ps, rec.data.data() + 1, sizeof(ps));
          session.send_state(ps);
          replaying_states = true;
        } else if (!rec.data.empty() && rec.data[0] == net::kRecVehicleState &&
                   rec.data.size() == 1 + sizeof(sync::VehicleState)) {
          sync::VehicleState vs;
          std::memcpy(&vs, rec.data.data() + 1, sizeof(vs));
          session.send_vehicle(vs);
        } else {
          session.send_game(rec.data, rec.reliable);
        }
        ++replay_next;
      }
      if (replay_next == replay.size()) std::printf("[%s] replay finished\n", a.name.c_str());
    }

    // Offline lane test (serve only): at 3 s create entity 999 (ours), at 5 s hand it to #1.
    if (a.serve && a.test_entity) {
      static int stage = 0;
      const std::uint32_t elapsed = now - started;
      if (stage == 0 && elapsed > 3000) {
        sync::EntityCreate c;
        c.id = 999, c.type = sync::EntityType::Ped, c.model = model_hash("F_Y_MULTIPLAYER"), c.sim_owner = 0;
        session.send_game(sync::encode_create(c), true);
        entities[999] = {sync::EntityType::Ped, 0, {}, 0, 0, 0, 0, 0};
        std::printf("[%s] test: created entity 999\n", a.name.c_str());
        stage = 1;
      } else if (stage == 1 && elapsed > 5000) {
        session.send_game(sync::encode_owner(999, 1), true);
        entities[999].owner = 1;
        std::printf("[%s] test: entity 999 -> #1\n", a.name.c_str());
        stage = 2;
      }
    }

    // Simulate and stream the entities we own (15 Hz).
    if (now - last_entity_send >= 66) {
      last_entity_send = now;
      for (auto& [id, e] : entities) {
        if (e.owner != my_id) continue;
        sync::EntityState s = e.last;
        if (e.type == sync::EntityType::Ped) {
          constexpr float kR = 2.0f, kV = 1.4f;
          e.angle += kV / kR * 0.066f;
          s.x = e.cx + std::cos(e.angle) * kR - kR, s.y = e.cy + std::sin(e.angle) * kR, s.z = e.cz;
          s.vx = -std::sin(e.angle) * kV, s.vy = std::cos(e.angle) * kV, s.vz = 0;
          sync::EntityState::set_heading(s, e.angle * 180.0f / kPi);
        } else {
          s.vx = s.vy = s.vz = 0;
        }
        s.id = id;
        s.seq = ++e.seq;
        s.time_ms = now + static_cast<std::uint32_t>(a.skew);
        e.last = s;
        session.send_game(sync::encode_entity_state(s), false);
      }
    }

    for (auto& [id, ip] : interps) ip.sample(now);

    const net::SessionStatus st = session.status();
    const bool active = st.mode == net::Mode::Joined || st.mode == net::Mode::Hosting;
    // --leave-after counts from the host's first state, not the connect: a host whose game
    // is paused (window in the background) accepts the connection but processes nothing.
    if (active && !was_joined) joined_at = 0;
    if (active && !joined_at && received[net::kHostId] > 0) {
      joined_at = now;
      if (a.leave_after > 0) std::printf("[%s] host is live; leaving in %d s\n", a.name.c_str(), a.leave_after);
    }
    if (active) was_joined = true;
    if (active && joined_at && a.leave_after > 0 && now - joined_at >= static_cast<std::uint32_t>(a.leave_after) * 1000) {
      std::printf("[%s] leaving after %d s, as asked\n", a.name.c_str(), a.leave_after);
      break;
    }
    if (!active || !have_center) continue;

    // Walk the circle at constant speed; heading follows the direction of travel.
    const bool paused = a.pause_every > 0 && ((now - started) / 1000) % static_cast<std::uint32_t>(a.pause_every) < 3;
    const float speed = (paused || dead_until || downed) ? 0.0f : a.speed; // the dead and downed don't walk
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

    // Combat: dead for 5 s (standing still), then respawn at full health.
    if (dead_until && now >= dead_until) {
      dead_until = 0, health = a.health;
      std::printf("[%s] respawned\n", a.name.c_str());
    }
    s.health = static_cast<std::uint16_t>(std::max(health, 0));
    if (downed) s.flags |= sync::PlayerState::kDowned;
    if (dead_until) {
      s.flags |= sync::PlayerState::kDead;
      s.vx = s.vy = s.vz = 0;
    } else if (a.armed) {
      s.weapon = kPistol;
      s.ax = cx, s.ay = cy, s.az = cz + 0.5f; // aim at the host
      s.flags |= sync::PlayerState::kAiming;
      if (a.shoot_host > 0 && now - last_shot_ms >= static_cast<std::uint32_t>(a.shoot_host) * 1000) {
        last_shot_ms = now;
        ++shots;
        damage_dealt += 10;
        session.send_game(sync::encode_damage({sync::DamageTarget::Player, net::kHostId, 10, kPistol}), true);
      }
    }
    s.shots = shots;
    if (!a.model.empty()) s.model = model_hash(a.model);

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

    if (now - last_send >= 33 && !replaying_states) { // a replay sends the recorded host's states instead
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
      for (const auto& [eid, e] : entities)
        line += std::format("  ent {}:owner#{}{}", eid, e.owner, e.owner == st.my_id ? "(us)" : "");
      if (entity_states_received) line += std::format("  entity states recv {}", entity_states_received);
      if (a.armed || damage_taken)
        line += std::format("  hp {}{}  dmg taken {} dealt {}", std::max(health, 0), dead_until ? " DEAD" : "",
                            damage_taken, damage_dealt);
      line += '\n';
      std::fwrite(line.data(), 1, line.size(), stdout);
      last_report_cost = net::steady_ms() - now;
    }
  }

  session.leave();
  std::printf("[%s] bye\n", a.name.c_str());
  return was_joined ? 0 : 2;
}
