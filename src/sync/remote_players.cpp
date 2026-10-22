#include "sync/remote_players.h"

#include <cmath>
#include <map>
#include <memory>
#include <mutex>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <format>
#include <string>

#include "core/input.h"
#include "game/phone.h"
#include "net/steam.h"
#include "net/steam_relay.h"
#include "core/log.h"
#include "game/script.h"
#include "sync/local_player.h"
#include "sync/player_state.h"
#include "sync/puppet.h"
#include "story/cutscene.h"
#include "sync/downed.h"
#include "story/dialogue.h"
#include "story/story_sync.h"
#include "sync/entity_sync.h"
#include "sync/vehicle_sync.h"

namespace ivc::coop {
namespace {
namespace n = natives;

struct Remote {
  std::string name;
  sync::Interpolator interp;
  sync::Puppet puppet;
  // Latest combat state, for status displays.
  std::uint16_t health = 0;
  std::uint32_t weapon = 0;
  bool dead = false;
  std::uint8_t wanted = 0;
  std::uint32_t vehicle_id = 0; // networked vehicle they're in (sync::kNoVehicle on foot)
  bool downed = false;
};

Settings g_cfg;
std::unique_ptr<net::Session> g_session; // created on first use, not inside DllMain
std::map<std::uint8_t, Remote> g_remotes;
sync::VehicleSync g_vehicles;
sync::EntitySync g_entities;
bool g_was_host = false;
std::uint32_t g_my_last_health = 0; // our player's health at the end of last frame

// Client mode (M6d).
bool g_want_client_mode = false; // joined: switch to the joiner character
bool g_client_mode = false;      // switched
bool g_restore_pending = false;  // left: switch back to the original character
std::uint32_t g_original_model = 0;

// Model swaps wait for the model to stream in; returns true once done.
bool swap_player_model(std::uint32_t model) {
  n::RequestModel(model);
  if (!n::HasModelLoaded(model)) return false;
  const n::Player player = n::GetPlayerId();
  n::ChangePlayerModel(player, model);
  n::SetCharDefaultComponentVariation(n::GetPlayerChar(player));
  n::MarkModelAsNoLongerNeeded(model);
  return true;
}

// Our own story's blips (contacts, mission starts, ...) were placed by scripts that are now
// frozen; a joiner should only see the host's. Hidden on entering client mode, restored
// exactly on leaving.
struct HiddenBlip {
  n::Blip blip;
  std::uint32_t display;
};
std::vector<HiddenBlip> g_hidden_blips;
constexpr std::uint32_t kBlipSprites = 128; // sprite ids to scan; IV uses fewer
constexpr std::uint32_t kSpritePlayer = 6, kSpriteNorth = 7, kSpriteWaypoint = 8;

void hide_own_blips(n::Ped me) {
  g_hidden_blips.clear();
  for (std::uint32_t sprite = 0; sprite < kBlipSprites; ++sprite) {
    for (n::Blip b = n::GetFirstBlipInfoId(sprite); n::DoesBlipExist(b); b = n::GetNextBlipInfoId(sprite)) {
      const std::uint32_t display = n::GetBlipInfoIdDisplay(b);
      if (display == 0) continue;
      const std::uint32_t type = n::GetBlipInfoIdType(b);
      const n::Ped ped = n::GetBlipInfoIdPedIndex(b);
      log::debug("Client mode: own blip {} sprite {} type {} display {} ped {}", b, sprite, type, display, ped);
      // Radar furniture and our own things stay: the player arrow (sprite 6), the north
      // marker (7) and our waypoint (8). The arrow is a coord blip, not one on our ped.
      if (ped == me || sprite == kSpritePlayer || sprite == kSpriteNorth || sprite == kSpriteWaypoint) continue;
      g_hidden_blips.push_back({b, display});
    }
  }
  for (const HiddenBlip& h : g_hidden_blips) n::ChangeBlipDisplay(h.blip, 0);
  log::info("Client mode: hid {} of our own blips", g_hidden_blips.size());
}

void restore_own_blips() {
  std::size_t restored = 0;
  for (const HiddenBlip& h : g_hidden_blips)
    if (n::DoesBlipExist(h.blip)) n::ChangeBlipDisplay(h.blip, h.display), ++restored;
  log::info("Client mode: restored {} of {} hidden blips", restored, g_hidden_blips.size());
  g_hidden_blips.clear();
}

void update_client_mode(n::Ped me) {
  if (!me) return;
  if (g_want_client_mode && !g_client_mode) {
    const std::uint32_t model = n::GetHashKey(g_cfg.joiner_model.c_str());
    const std::uint32_t original = n::GetCharModel(me);
    if (swap_player_model(model)) {
      g_original_model = original;
      g_client_mode = true;
      if (g_cfg.freeze_story_scripts) script::set_suppress_predicate(story::is_story_script);
      hide_own_blips(me);
      log::info("Client mode: playing as {} (was 0x{:08X}); story scripts {}", g_cfg.joiner_model, original,
                g_cfg.freeze_story_scripts ? "frozen" : "left running");
    }
  }
  if (g_restore_pending && g_original_model) {
    if (swap_player_model(g_original_model)) {
      g_restore_pending = false;
      g_client_mode = false;
      restore_own_blips();
      log::info("Client mode ended: back to model 0x{:08X}", g_original_model);
    }
  }
}
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

// M4 test bed (host only): spawn networked entities in front of the player.
struct PendingSpawn {
  sync::EntityType type;
  std::uint32_t model;
};
std::optional<PendingSpawn> g_pending_spawn;
bool g_spawn_ped_down = true, g_spawn_car_down = true, g_delete_down = true;

bool g_record_down = true;
std::uint32_t g_record_started_ms = 0;

void handle_entity_keys(bool hosting) {
  if (!hosting) return;
  // Test rig (M6c): record everything we send, for a bot to replay to a client later.
  if (input::pressed_ctrl_shift('R', g_record_down)) {
    if (session().recording()) {
      session().stop_recording();
      log::info("Recording stopped");
    } else {
      SYSTEMTIME t;
      GetLocalTime(&t);
      wchar_t dir[MAX_PATH];
      GetModuleFileNameW(GetModuleHandleW(L"iv-coop.asi"), dir, MAX_PATH);
      std::wstring d(dir);
      d = d.substr(0, d.find_last_of(L"\\/") + 1);
      std::string path;
      for (wchar_t ch : d) path.push_back(static_cast<char>(ch));
      path += std::format("iv-coop-{:02}{:02}{:02}.ivcrec", t.wHour, t.wMinute, t.wSecond);
      if (session().start_recording(path))
        log::info("Recording session to {}", path), g_record_started_ms = net::steady_ms();
      else
        log::error("Could not open recording {}", path);
    }
  }
  if (input::pressed_ctrl_shift('P', g_spawn_ped_down))
    g_pending_spawn = PendingSpawn{sync::EntityType::Ped, n::GetHashKey("F_Y_MULTIPLAYER")};
  if (input::pressed_ctrl_shift('V', g_spawn_car_down))
    g_pending_spawn = PendingSpawn{sync::EntityType::Vehicle, n::GetHashKey("TAXI")};
  if (input::pressed_ctrl_shift('X', g_delete_down)) g_entities.host_delete_all(session());
}

void process_pending_spawn(n::Ped me) {
  if (!g_pending_spawn || !me) return;
  const auto [type, model] = *g_pending_spawn;
  n::RequestModel(model);
  if (!n::HasModelLoaded(model)) return;
  const n::Vector3 at = n::GetOffsetFromCharInWorldCoords(me, {0.0f, type == sync::EntityType::Ped ? 4.0f : 7.0f, 0.0f});
  n::Handle h = type == sync::EntityType::Ped ? n::CreateChar(n::kPedTypeCivMale, model, at) : n::CreateCar(model, at);
  n::MarkModelAsNoLongerNeeded(model);
  g_pending_spawn.reset();
  if (!h) {
    log::error("Test entity spawn failed");
    return;
  }
  if (type == sync::EntityType::Ped) n::TaskWanderStandard(h);
  g_entities.host_register(h, type, model, session());
}

void handle_hotkeys() {
  const net::Mode mode = session().status().mode;
  handle_entity_keys(mode == net::Mode::Hosting);
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

// This PC's LAN IPv4 (first non-loopback), for the lobby's address until Steam relay
// carries the traffic; falls back to the configured bind address.
std::string local_ipv4() {
  char host[256] = {};
  if (gethostname(host, sizeof(host)) != 0) return g_cfg.bind_address;
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  if (getaddrinfo(host, nullptr, &hints, &res) != 0) return g_cfg.bind_address;
  std::string out = g_cfg.bind_address;
  for (addrinfo* a = res; a; a = a->ai_next) {
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr, buf, sizeof(buf));
    if (std::string(buf).rfind("127.", 0) != 0) {
      out = buf;
      break;
    }
  }
  freeaddrinfo(res);
  return out;
}

// The phone's Multiplayer menu (game/phone) and Steam lobbies (net/steam).
std::string g_password; // of the session we host / are joining
// Host self-check: search for our own lobby by password a few seconds after creating it
// (lobby data takes a moment to be published; an immediate search found nothing).
std::vector<std::uint32_t> g_self_checks;

void handle_phone() {
  // No name set in the ini: use the Steam name (once Steam is up).
  static bool named = false;
  if (!named && steam::available()) {
    named = true;
    if (g_cfg.name == "Player" || g_cfg.name.empty()) {
      const std::string persona = steam::persona_name();
      if (!persona.empty()) g_cfg.name = persona.substr(0, 31);
    }
  }
  net::Session& net = session();
  const net::Mode mode = net.status().mode;
  phone::set_in_session(mode != net::Mode::Idle);
  steam::tick();
  steam_relay::tick();

  if (auto req = phone::poll()) {
    switch (req->action) {
      case phone::Action::Host:
        if (mode != net::Mode::Idle) break;
        if (!net.host(g_cfg.port, g_cfg.name, g_cfg.bind_address)) {
          phone::notify("Couldn't host: " + net.status().last_error);
          break;
        }
        g_password = req->password;
        log::info("Hosting on UDP {} as '{}' (phone, password {})", g_cfg.port, g_cfg.name, g_password);
        if (steam::available()) {
          steam::host_lobby(g_password, std::format("{}:{}", local_ipv4(), g_cfg.port));
          steam_relay::start_host(g_cfg.port);
          phone::notify("Starting co-op...");
        } else {
          phone::notify("Hosting co-op (Steam unavailable: friends join by address).");
        }
        break;
      case phone::Action::Join:
        if (mode != net::Mode::Idle) break;
        if (!steam::available()) {
          phone::notify("Joining by password needs Steam.");
          break;
        }
        g_password = req->password;
        steam::find_lobby(g_password);
        phone::notify("Looking for a co-op game with password " + g_password + "...");
        break;
      case phone::Action::Leave:
        if (mode != net::Mode::Idle) {
          log::info("Leaving session ({}, phone)", net::to_string(mode));
          net.leave();
        }
        steam_relay::stop();
        steam::leave_lobby();
        phone::notify("Left co-op.");
        break;
    }
  }

  if (!g_self_checks.empty() && GetTickCount() >= g_self_checks.front()) {
    g_self_checks.erase(g_self_checks.begin());
    if (net.status().mode == net::Mode::Hosting) steam::find_lobby(g_password);
  }

  while (auto r = steam::poll()) {
    switch (r->kind) {
      case steam::LobbyResult::Created: {
        phone::notify("Co-op ready. Friends join with password " + g_password + ".");
        const std::uint32_t now = GetTickCount();
        g_self_checks = {now + 5000, now + 15000};
        break;
      }
      case steam::LobbyResult::CreateFailed:
        phone::notify("Co-op is hosting, but Steam couldn't list it.");
        break;
      case steam::LobbyResult::Found:
        if (net.status().mode == net::Mode::Hosting) {
          log::info("Steam self-check: our lobby is findable by its password");
          break;
        }
        // Enter the host's lobby: it then accepts our P2P traffic (see steam_relay).
        steam::join_lobby(r->lobby);
        phone::notify("Found " + r->host_name + "'s game. Connecting...");
        break;
      case steam::LobbyResult::Joined: {
        const auto port = steam_relay::start_join(r->owner);
        if (port && net.join("127.0.0.1", *port, g_cfg.name)) {
          log::info("Joining host {} through the Steam relay (local port {})", r->owner, *port);
        } else {
          phone::notify("Couldn't connect: " + net.status().last_error);
          steam_relay::stop();
          steam::leave_lobby();
        }
        break;
      }
      case steam::LobbyResult::JoinFailed:
        phone::notify("Couldn't enter that game's lobby.");
        break;
      case steam::LobbyResult::NotFound:
        if (net.status().mode == net::Mode::Hosting)
          log::warn("Steam self-check: our own lobby wasn't found by its password");
        else
          phone::notify("No co-op game found with password " + g_password + ".");
        break;
    }
  }
}

// Joiner during a replicated cutscene: IV casts the local player as the protagonist, so
// switch to the protagonist's model for it, and put the player back where they were.
n::Vector3 g_cutscene_return{};
bool cutscene_begin() {
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me) return true;
  static bool saved = false;
  if (!saved) g_cutscene_return = n::GetCharCoordinates(me), saved = true;
  if (!swap_player_model(0x6F0783F5)) return false; // "player": streaming, try next frame
  saved = false;
  return true;
}
bool cutscene_end() {
  const std::uint32_t model = g_client_mode && !g_restore_pending ? n::GetHashKey(g_cfg.joiner_model.c_str())
                                                                    : g_original_model;
  if (model && !swap_player_model(model)) return false;
  if (const n::Ped me = n::GetPlayerChar(n::GetPlayerId()); me && (g_cutscene_return.x || g_cutscene_return.y))
    n::SetCharCoordinatesNoOffset(me, g_cutscene_return);
  return true;
}

// Other players' puppets stay out of shot while a cutscene plays here.
bool g_puppets_hidden = false;
void hide_puppets_in_cutscenes() {
  const bool hide = story::cutscene::playing();
  if (hide == g_puppets_hidden) return;
  g_puppets_hidden = hide;
  for (auto& [id, r] : g_remotes)
    if (const n::Ped p = r.puppet.ped(); p && n::DoesCharExist(p)) n::SetCharVisible(p, !hide);
}

// Mission-provided vehicles (ARCHITECTURE): when the host's mission hands the protagonist
// a car, partners never touch it; the lowest-id joiner spawns a duplicate (same model and
// colours) on the nearest road node and drives, the other joiners warp into its passenger
// seats. The duplicate goes when the mission lets go of its car (left as a normal car if
// someone is in it).
struct MissionCar {
  n::Vehicle duplicate = 0; // ours, if we're the one who spawned it
  bool boarded = false;     // passengers: we're in the lead joiner's duplicate
  DWORD since = GetTickCount();
};
std::map<std::uint32_t, MissionCar> g_mission_cars; // by the mission car's entity id

n::Vehicle spawn_duplicate(n::Vehicle source) {
  const std::uint32_t model = n::GetCarModel(source);
  n::RequestModel(model);
  if (!n::HasModelLoaded(model)) return 0; // streaming: next frame
  const n::Vector3 src = n::GetCarCoordinates(source);
  const float src_heading = n::GetCarHeading(source);
  n::Vector3 at{};
  float heading = src_heading;
  constexpr float kMinGap = 7.0f;
  if (!n::GetClosestCarNodeWithHeading({src.x + 10.0f, src.y + 10.0f, src.z}, at, heading) ||
      std::hypot(at.x - src.x, at.y - src.y) < kMinGap) {
    const float rad = src_heading * 3.14159265f / 180.0f; // behind the mission car
    at = {src.x + std::sin(rad) * 8.0f, src.y - std::cos(rad) * 8.0f, src.z};
    heading = src_heading;
  }
  const n::Vehicle car = n::CreateCar(model, at);
  n::MarkModelAsNoLongerNeeded(model);
  if (!car) return 0;
  n::invoke(0x75E40528, car, heading); // SET_CAR_HEADING
  std::int32_t c1 = 0, c2 = 0, c3 = 0, c4 = 0;
  n::GetCarColours(source, c1, c2);
  n::GetExtraCarColours(source, c3, c4);
  n::ChangeCarColour(car, c1, c2);
  n::SetExtraCarColours(car, c3, c4);
  return car;
}

void handle_mission_cars(std::uint8_t my_id) {
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me) return;
  for (std::uint32_t id : story::mission_cars(g_entities)) g_mission_cars.try_emplace(id);
  // The lead joiner (lowest id other than the host) drives the duplicate.
  std::uint8_t lead = my_id;
  for (const auto& [id, r] : g_remotes)
    if (id != net::kHostId && id < lead) lead = id;
  for (auto it = g_mission_cars.begin(); it != g_mission_cars.end();) {
    MissionCar& mc = it->second;
    const n::Vehicle source = g_entities.handle_for_id(it->first);
    if (!source || !n::DoesVehicleExist(source)) { // the mission let go of its car
      if (mc.duplicate && n::DoesVehicleExist(mc.duplicate)) {
        if (n::IsCharInCar(me, mc.duplicate) || n::GetDriverOfCar(mc.duplicate))
          n::invoke(0x20C76FD1, &mc.duplicate); // MARK_CAR_AS_NO_LONGER_NEEDED: a normal car now
        else
          n::DeleteCar(mc.duplicate);
      }
      it = g_mission_cars.erase(it);
      continue;
    }
    if (lead == my_id) {
      if (!mc.duplicate) {
        mc.duplicate = spawn_duplicate(source);
        if (mc.duplicate) {
          if (n::IsCharInAnyCar(me)) n::WarpCharFromCarToCoord(me, n::GetCharCoordinates(me));
          n::WarpCharIntoCar(me, mc.duplicate);
          log::info("Mission car: spawned a duplicate of entity {} for the partners", it->first);
        }
      }
    } else if (!mc.boarded && GetTickCount() - mc.since < 30000) {
      // Ride in the lead joiner's duplicate once it shows up as their vehicle.
      const auto lead_it = g_remotes.find(lead);
      const n::Vehicle dup = lead_it != g_remotes.end() ? g_vehicles.handle_for(lead_it->second.vehicle_id) : 0;
      if (dup && n::DoesVehicleExist(dup)) {
        const std::uint32_t seats = n::GetMaximumNumberOfPassengers(dup);
        for (std::uint32_t seat = 0; seat < seats && seat < 8; ++seat)
          if (n::IsCarPassengerSeatFree(dup, seat)) {
            if (n::IsCharInAnyCar(me)) n::WarpCharFromCarToCoord(me, n::GetCharCoordinates(me));
            n::WarpCharIntoCarAsPassenger(me, dup, seat);
            mc.boarded = true;
            log::info("Mission car: riding in the lead joiner's duplicate (seat {})", seat);
            break;
          }
      }
    }
    ++it;
  }
}

void configure_cutscenes() {
  story::cutscene::set_callbacks({cutscene_begin, cutscene_end});
}

} // namespace

void configure(const Settings& s) {
  g_cfg = s;
  configure_cutscenes();
  story::dialogue::set_host_ped_provider([] {
    const auto it = g_remotes.find(net::kHostId);
    return it != g_remotes.end() ? it->second.puppet.ped() : 0;
  });
  g_vehicles.interp_delay_ms = s.interp_delay_ms;
  g_vehicles.physics_follow = s.physics_follow;
  g_vehicles.force_type = s.vehicle_force_type;
}

void tick() {
  handle_hotkeys();
  handle_phone();
  story::cutscene::client_tick(); // also after leaving, to finish a restore
  hide_puppets_in_cutscenes();
  {
    downed::Frame f;
    const net::SessionStatus st = session().status();
    f.in_session = st.mode == net::Mode::Hosting || st.mode == net::Mode::Joined;
    f.hosting = st.mode == net::Mode::Hosting;
    f.my_id = st.my_id;
    for (const auto& [id, r] : g_remotes)
      for (const auto& p : st.players)
        if (p.id == id && id != st.my_id) f.teammates.push_back({id, r.name, r.puppet.ped(), r.downed});
    downed::tick(f, session());
  }
  net::Session& net = session();

  for (const auto& e : net.drain_events()) {
    using K = net::SessionEvent::Kind;
    switch (e.kind) {
      case K::Joined:
        log::info("Player {} '{}' joined", e.id, e.text);
        remote(e.id).name = e.text;
        if (net.status().mode == net::Mode::Hosting) {
          g_entities.on_player_joined(e.id, net);
          story::host_on_player_joined(e.id, net);
        }
        break;
      case K::Left:
        log::info("Player {} '{}' left", e.id, e.text);
        drop_remote(e.id);
        g_vehicles.drop_owner(e.id);
        g_entities.on_player_left(e.id);
        break;
      case K::Connected:
        log::info("Connected; we are player {}", e.id);
        g_want_client_mode = true;
        break;
      case K::Disconnected:
        log::info("Disconnected: {}", e.text);
        drop_all();
        g_vehicles.clear();
        g_entities.clear(g_was_host);
        story::clear();
        g_mission_cars.clear();
        steam_relay::stop();
        steam::leave_lobby();
        if (g_want_client_mode || g_client_mode) {
          script::set_suppress_predicate(nullptr); // our story scripts resume where they were
          g_want_client_mode = false;
          g_restore_pending = g_client_mode;
        }
        break;
    }
  }

  const std::uint32_t now = net::steady_ms();
  const net::SessionStatus ss = net.status();
  const bool active = ss.mode == net::Mode::Hosting || ss.mode == net::Mode::Joined;
  {
    const n::Ped p = n::GetPlayerChar(n::GetPlayerId());
    update_client_mode(p && n::DoesCharExist(p) ? p : 0);
  }
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

  // Only players currently in the session count. Packets from a player who just left can
  // still be queued behind their "left" event; accepting them would resurrect a frozen
  // ghost of that player (and keep entities it owned from being reassigned).
  const auto in_session = [&ss](std::uint8_t id) {
    for (const auto& p : ss.players)
      if (p.id == id) return true;
    return false;
  };
  for (auto it = g_remotes.begin(); it != g_remotes.end();) {
    if (!in_session(it->first)) {
      it->second.puppet.despawn();
      it = g_remotes.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& u : net.drain_states())
    if (in_session(u.id)) remote(u.id).interp.push(u.state, u.arrival_ms);
  for (const auto& u : net.drain_vehicles())
    if (in_session(u.id)) g_vehicles.push_remote(u);
  g_vehicles.update_remote(now, me); // stand-in cars first, so puppets can be seated in them

  // Everyone's position, for entity sim-ownership decisions (host).
  std::vector<sync::PlayerPos> positions;
  if (local) positions.push_back({ss.my_id, {local->x, local->y, local->z}});

  std::vector<RemoteStatus> remotes;
  for (auto& [id, r] : g_remotes) {
    float dist = 0;
    if (auto smp = r.interp.sample(now)) { // once per frame: sampling advances the timeline
      const n::Vector3 at{smp->x, smp->y, smp->z};
      positions.push_back({id, at});
      r.puppet.set_model(smp->model); // their real model (host: Niko); 0 -> configured fallback
      if (r.puppet.ensure(at)) {
        r.puppet.apply_outfit(*smp);
        const n::Vehicle veh = g_vehicles.handle_for(smp->vehicle_id);
        if (veh) {
          r.puppet.sit_in(veh, smp->seat);
        } else {
          // On foot, or their car hasn't arrived here yet: follow their position on foot.
          if (r.puppet.in_vehicle()) r.puppet.leave_vehicle(at);
          r.puppet.drive(*smp, now);
        }
        r.puppet.apply_combat(*smp, now);
        r.health = smp->health, r.weapon = smp->weapon, r.dead = (smp->flags & sync::PlayerState::kDead) != 0;
        r.wanted = smp->wanted;
        r.vehicle_id = smp->vehicle_id;
        r.downed = (smp->flags & sync::PlayerState::kDowned) != 0;
        r.puppet.set_downed_marker(r.downed);
      }
      if (local) dist = std::hypot(smp->x - local->x, smp->y - local->y, smp->z - local->z);
    }
    // Our hits on their puppet go to them; they apply the damage to themselves.
    if (active && me) {
      if (const std::uint16_t dmg = r.puppet.take_hits_from(me)) {
        net.send_game(sync::encode_damage({sync::DamageTarget::Player, id, dmg, local ? local->weapon : 0}), true);
        log::info("Hit player #{} '{}' for {}", id, r.name, dmg);
      }
    }
    const auto& st = r.interp.stats();
    const unsigned frames = st.interpolated + st.extrapolated;
    std::uint32_t rtt = 0;
    for (const auto& p : ss.players)
      if (p.id == id) rtt = p.rtt_ms;
    remotes.push_back({id, r.name, rtt, r.puppet.spawned(),
                       frames ? 100.0f * static_cast<float>(st.extrapolated) / static_cast<float>(frames) : 0.0f,
                       dist, r.puppet.phase(), r.puppet.error_m(), r.puppet.snaps(), r.health, r.weapon, r.dead,
                       r.wanted});
  }

  // Combat: damage our player took from a puppet's local bullets is undone; the shooter
  // reports the real hit (which arrives as a Damage message below).
  if (active && me) {
    bool by_puppet = false;
    for (const auto& [id, r] : g_remotes)
      if (r.puppet.ped() && n::HasCharBeenDamagedByChar(me, r.puppet.ped())) by_puppet = true;
    if (by_puppet && g_my_last_health) {
      n::SetCharHealth(me, g_my_last_health);
      n::ClearCharLastDamageEntity(me);
    }
  }

  // Networked entities (M4) and combat messages (M5).
  if (active) {
    g_was_host = ss.mode == net::Mode::Hosting;
    process_pending_spawn(me);
    for (const auto& m : net.drain_game()) {
      if (!in_session(m.from)) continue;
      if (downed::is_msg(m.data)) {
        downed::on_message(m, ss.my_id);
        continue;
      }
      const auto msg = sync::decode_entity_msg(m.data);
      if (msg && msg->type == sync::GameMsg::Damage) {
        const auto& d = msg->damage;
        if (d.kind == sync::DamageTarget::Player && d.target == ss.my_id && me && !n::IsCharDead(me)) {
          const std::uint32_t h = n::GetCharHealth(me);
          n::SetCharHealth(me, downed::floor_health(h > d.amount ? h - d.amount : 0));
          log::info("Took {} damage from player #{} (weapon {}), health {} -> {}", d.amount, m.from, d.weapon, h,
                    h > d.amount ? h - d.amount : 0);
        } else if (d.kind == sync::DamageTarget::Entity) {
          g_entities.apply_damage(d.target, d.amount, ss.my_id);
        }
        continue;
      }
      if (story::client_on_message(m, g_entities)) continue; // story text/blips from the host
      g_entities.on_message(m, ss.my_id);
    }
    // Story (M6): the host sends what its story scripts did this frame; clients retry
    // blips whose entity copy wasn't there yet.
    story::host_tick(g_was_host, net, g_entities);
    if (!g_was_host) {
      story::client_tick(g_entities);
      handle_mission_cars(ss.my_id);
    }
    g_entities.tick(now, ss.my_id, g_was_host, positions, net);
    for (const auto& hit : g_entities.collect_hits(me)) {
      net.send_game(sync::encode_damage({hit.kind, hit.target, hit.amount, local ? local->weapon : 0}), true);
      log::info("Hit entity {} for {}", hit.target, hit.amount);
    }
  }
  if (!active) story::host_tick(false, net, g_entities); // no session: capture stays off
  if (me) g_my_last_health = n::GetCharHealth(me);
  const auto entities = g_entities.status();
  static std::uint32_t last_elog = now;
  if (!entities.empty() && now - last_elog >= 10000) {
    last_elog = now;
    for (const auto& e : entities)
      log::info("Entity {} ({}): sim owner #{}{}, at {:.1f} {:.1f} {:.1f}, {} migrations", e.id,
                e.type == sync::EntityType::Ped ? "ped" : "vehicle", e.sim_owner, e.live ? " (live here)" : "", e.x,
                e.y, e.z, e.migrations);
  }

  static std::uint32_t last_log = now;
  if (!remotes.empty() && now - last_log >= 10000) {
    last_log = now;
    for (const auto& r : remotes)
      log::info("Remote #{} '{}': {} err {:.1f} m, {} snaps, {:.1f} m away, extrap {:.1f}%, hp {}{}, weapon {}, "
                "wanted {}",
                r.id, r.name, r.action, r.error_m, r.snaps, r.distance_m, r.extrapolated_pct, r.health,
                r.dead ? " (DEAD)" : "", r.weapon, r.wanted);
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
  g_status.entities = entities;
  g_status.recording = net.recording();
  g_status.recording_ms = g_status.recording ? now - g_record_started_ms : 0;
  g_status.story_messages = story::status().messages;
}

Status status() {
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

} // namespace ivc::coop
