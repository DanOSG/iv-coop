#include "sync/remote_players.h"

#include <cmath>
#include <map>
#include <memory>
#include <filesystem>
#include <mutex>
#include <optional>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <format>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <string>

#include "core/input.h"
#include "core/log_export.h"
#include "game/episode.h"
#include "game/phone.h"
#include "net/steam.h"
#include "net/steam_relay.h"
#include "net/upnp.h"
#include "core/log.h"
#include "game/script.h"
#include "sync/local_player.h"
#include "sync/player_state.h"
#include "sync/puppet.h"
#include "story/cutscene.h"
#include "sync/cutscene_double.h"
#include "story/scene_cam.h"
#include "sync/downed.h"
#include "sync/episode_sync.h"
#include "sync/activities/activities.h"
#include "sync/activities/darts.h"
#include "sync/activities/races.h"
#include "sync/activity_seats.h"
#include "sync/player_mirror.h"
#include "sync/race_sync.h"
#include "sync/invasion.h"
#include "story/story_protocol.h"
#include "story/fx.h"
#include "sync/attach_sync.h"
#include "sync/bowling_sync.h"
#include "sync/drunk_sync.h"
#include "story/pickups.h"
#include "sync/radio_sync.h"
#include "sync/regroup.h"
#include "sync/session_settings.h"
#include "sync/wallet.h"
#include "sync/character.h"
#include "sync/scream.h"
#include "sync/ped_looks.h"
#include "sync/vehicle_props.h"
#include "sync/team_fire.h"
#include "sync/melee_sync.h"
#include "story/dialogue.h"
#include "story/sounds.h"
#include "story/npc_anims.h"
#include "story/story_sync.h"
#include "sync/ambient_sync.h"
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
  downed::InjuredPose pose; // their puppet lying hurt while they're down
};

Settings g_cfg;
std::unique_ptr<net::Session> g_session; // created on first use, not inside DllMain
std::map<std::uint8_t, Remote> g_remotes;
sync::VehicleSync g_vehicles;
sync::EntitySync g_entities;
bool g_was_host = false;
std::uint32_t g_my_last_health = 0; // our player's health at the end of last frame

// Invasions (M11): our role in the session (Coop unless we joined as an invader), and the phone's Invade in progress.
net::Role g_my_role = net::Role::Coop;
bool g_invading = false;        // Invade chosen: the Steam search / join that follows is as an invader
bool g_lobby_flag = false;      // host: the lobby's ivc_open as last set
bool g_invasion_finish = false; // invader: the session ended; restore our own game once the model is back
bool invader() { return g_my_role == net::Role::Invader; }

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
  const n::Ped ped = n::GetPlayerChar(player);
  n::SetCharDefaultComponentVariation(ped);
  if (character::is_own_model(model)) character::apply_own(ped); // the joiner's saved character (M9)
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
constexpr std::uint32_t kBlipSprites = 256; // sprite ids to scan; IV uses fewer, the episodes add their own
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

// The character a joiner plays: their own (character creator / ini), or an FBI agent for an invader.
std::uint32_t joiner_model_hash() {
  if (invader())
    if (const std::uint32_t m = invasion::invader_model()) return m;
  return character::joiner_model(n::GetHashKey(g_cfg.joiner_model.c_str()));
}

// joiner_model_hash(), if this episode's image has it; a model that is missing would never finish streaming and leave
// the joiner stuck as the protagonist, so fall back to the stock multiplayer model, else `fallback` (no swap).
std::uint32_t joiner_model_checked(std::uint32_t fallback) {
  const std::uint32_t model = joiner_model_hash();
  if (n::IsModelInCdimage(model) && n::IsThisModelAPed(model)) return model;
  static std::uint32_t warned = 0;
  const std::uint32_t stock = n::GetHashKey("M_Y_MULTIPLAYER");
  const bool have_stock = n::IsModelInCdimage(stock) && n::IsThisModelAPed(stock);
  if (warned != model) {
    warned = model;
    log::warn("Client mode: model 0x{:08X} isn't a ped model in {}; using {}", model, episode::name(episode::current()),
              have_stock ? "M_Y_MULTIPLAYER" : "the protagonist");
  }
  return have_stock ? stock : fallback;
}

void update_client_mode(n::Ped me) {
  if (!me) return;
  if (g_want_client_mode && !g_client_mode) {
    const std::uint32_t original = n::GetCharModel(me);
    const std::uint32_t model = joiner_model_checked(original);
    if (model == original || swap_player_model(model)) {
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
      if (g_invasion_finish) g_invasion_finish = false, invasion::finish();
    }
  }
}
bool g_host_was_down = true, g_join_was_down = true, g_invade_was_down = true;
std::uint32_t g_last_send = 0;
std::uint16_t g_seq = 0;

std::mutex g_status_mutex;
Status g_status;

net::Session& session() {
  if (!g_session) {
    g_session = std::make_unique<net::Session>();
    g_session->set_logger([](int level, const std::string& text) {
      if (level >= 1) log::warn("[Net] {}", text);
      else log::info("[Net] {}", text);
    });
  }
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
    character::forget(id);
  }
}

void drop_all() {
  for (auto& [id, r] : g_remotes) r.puppet.despawn(), character::forget(id);
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

// Save logs (phone: Multiplayer > Save logs, or Ctrl+Shift+L): zipped to the Desktop on a worker thread, the result
// shown on the phone when it's ready.
std::mutex g_logs_mutex;
std::optional<std::string> g_logs_done;
bool g_logs_busy = false;
bool g_logs_key_down = false;

void save_logs() {
  if (g_logs_busy) return;
  g_logs_busy = true;
  phone::notify("Saving logs...");
  log::info("Saving logs to the Desktop");
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::filesystem::path game = std::filesystem::path(exe).parent_path();
  std::thread([game] {
    const log_export::Result r = log_export::run(game, IVCOOP_VERSION);
    std::lock_guard lock(g_logs_mutex);
    g_logs_done = r.message;
  }).detach();
}

void poll_save_logs() {
  std::optional<std::string> done;
  {
    std::lock_guard lock(g_logs_mutex);
    done.swap(g_logs_done);
  }
  if (!done) return;
  g_logs_busy = false;
  log::info("{}", *done);
  phone::notify(*done);
}

void handle_hotkeys() {
  if (input::pressed_ctrl_shift('L', g_logs_key_down)) save_logs();
  poll_save_logs();
  const net::Mode mode = session().status().mode;
  handle_entity_keys(mode == net::Mode::Hosting);
  const bool no_phone = phone::unavailable(); // F10/F11 stand in for the phone menu (handle_phone hosts)
  if (!no_phone && input::dev_pressed(g_cfg.host_key, g_host_was_down)) {
    if (mode == net::Mode::Idle) {
      if (session().host(g_cfg.port, g_cfg.name, g_cfg.bind_address)) {
        log::info("Hosting on UDP {} as '{}'", g_cfg.port, g_cfg.name);
        upnp::open(g_cfg.port);
      } else
        log::error("Hosting failed: {}", session().status().last_error);
    } else {
      log::info("Leaving session ({})", net::to_string(mode));
      session().leave();
    }
  }
  if (input::dev_pressed(g_cfg.invade_key, g_invade_was_down)) {
    if (mode == net::Mode::Idle) {
      if (session().join(g_cfg.join_address, g_cfg.port, g_cfg.name, net::Role::Invader))
        log::info("Invading {}:{} as '{}'", g_cfg.join_address, g_cfg.port, g_cfg.name);
      else
        log::error("Invade failed: {}", session().status().last_error);
    } else {
      log::info("Leaving session ({})", net::to_string(mode));
      session().leave();
    }
  }
  if (no_phone ? input::pressed(g_cfg.join_key, g_join_was_down) : input::dev_pressed(g_cfg.join_key, g_join_was_down)) {
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

// Joiners aren't pulled along by the host's mission teleports; this brings them over:
// into a free passenger seat if the host is in a vehicle, else next to them.
void go_to_host() {
  const auto it = g_remotes.find(net::kHostId);
  const n::Ped host = it != g_remotes.end() ? it->second.puppet.ped() : 0;
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!host || !me || !n::DoesCharExist(host)) {
    phone::notify("Can't find the host yet.");
    return;
  }
  if (n::IsCharInAnyCar(me)) n::WarpCharFromCarToCoord(me, n::GetCharCoordinates(me));
  if (n::IsCharInAnyCar(host)) {
    const n::Vehicle car = n::StoreCarCharIsInNoSave(host);
    const std::uint32_t seats = car ? n::GetMaximumNumberOfPassengers(car) : 0;
    for (std::uint32_t seat = 0; seat < seats && seat < 8; ++seat)
      if (n::IsCarPassengerSeatFree(car, seat)) {
        n::WarpCharIntoCarAsPassenger(me, car, seat);
        log::info("Go to host: passenger seat {}", seat);
        return;
      }
  }
  n::SetCharCoordinatesNoOffset(me, n::GetOffsetFromCharInWorldCoords(host, {1.5f, -1.5f, 0.0f}));
  log::info("Go to host: next to them");
}

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
  phone::set_role(mode == net::Mode::Hosting ? phone::Role::Host
                  : mode == net::Mode::Idle  ? phone::Role::None
                  : invader()                ? phone::Role::Invader
                                             : phone::Role::Joiner);
  steam::tick();
  steam_relay::tick();

  // The host is in another episode (message 67): leave and say which one to start.
  if (auto why = episode_sync::take_refusal()) {
    g_invading = false;
    if (mode != net::Mode::Idle) {
      log::info("Leaving session (different episode)");
      net.leave();
    }
    steam_relay::stop();
    steam::leave_lobby();
    upnp::close();
    phone::notify(*why);
  }

  auto req = phone::poll();
  if (!req && phone::unavailable()) {
    // No phone menu in this episode (its script layout isn't mapped or didn't check out): F10 hosts with a random
    // password / leaves, F11 joins [Net] JoinAddress / leaves (handle_hotkeys).
    static bool host_down = true, told = false;
    if (!told) {
      told = true;
      phone::notify(std::format("The co-op phone menu isn't available in {} yet. Press F10 to host (a password is shown) "
                                "or F11 to join the address in iv-coop.ini.",
                                episode::name(episode::current())));
    }
    if (input::pressed(g_cfg.host_key, host_down)) {
      if (mode == net::Mode::Idle) {
        static std::mt19937 rng{std::random_device{}()};
        req = phone::Request{phone::Action::Host, std::format("{:06}", 100000 + rng() % 900000)};
      } else {
        req = phone::Request{phone::Action::Leave, {}};
      }
    }
  }
  if (req) {
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
          upnp::open(g_cfg.port); // direct joins only: ask the router to forward the port
          phone::notify("Hosting co-op (Steam unavailable: friends join by address). Opening the port on your router...");
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
        g_invading = false;
        if (mode != net::Mode::Idle) {
          log::info("Leaving session ({}, phone)", net::to_string(mode));
          net.leave();
        }
        steam_relay::stop();
        steam::leave_lobby();
        upnp::close();
        phone::notify("Left co-op.");
        break;
      case phone::Action::GoToHost:
        go_to_host();
        break;
      case phone::Action::SaveLogs:
        save_logs();
        break;
      case phone::Action::Invade:
        if (mode != net::Mode::Idle) break;
        if (!steam::available()) {
          phone::notify("Invading needs Steam.");
          break;
        }
        g_invading = true;
        steam::find_open_lobby();
        phone::notify("Looking for a game to invade...");
        break;
    }
  }

  if (const auto msg = upnp::poll()) phone::notify(*msg);

  if (!g_self_checks.empty() && GetTickCount() >= g_self_checks.front()) {
    g_self_checks.erase(g_self_checks.begin());
    if (net.status().mode == net::Mode::Hosting) steam::find_lobby(g_password);
  }

  while (auto r = steam::poll()) {
    switch (r->kind) {
      case steam::LobbyResult::Created: {
        g_lobby_flag = false; // new lobbies start closed to invasion
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
        phone::notify(g_invading ? "Found " + r->host_name + "'s game. Invading..." : "Found " + r->host_name + "'s game. Connecting...");
        break;
      case steam::LobbyResult::Joined: {
        const auto port = steam_relay::start_join(r->owner);
        const bool as_invader = std::exchange(g_invading, false);
        if (port && net.join("127.0.0.1", *port, g_cfg.name, as_invader ? net::Role::Invader : net::Role::Coop)) {
          log::info("{} host {} through the Steam relay (local port {})", as_invader ? "Invading" : "Joining", r->owner, *port);
        } else {
          phone::notify("Couldn't connect: " + net.status().last_error);
          steam_relay::stop();
          steam::leave_lobby();
        }
        break;
      }
      case steam::LobbyResult::JoinFailed:
        g_invading = false;
        phone::notify("Couldn't enter that game's lobby.");
        break;
      case steam::LobbyResult::NotFound:
        if (net.status().mode == net::Mode::Hosting)
          log::warn("Steam self-check: our own lobby wasn't found by its password");
        else if (std::exchange(g_invading, false))
          phone::notify("No game is open to invasion right now.");
        else
          phone::notify("No co-op game found with password " + g_password + ".");
        break;
    }
  }
}

// Joiner during a replicated cutscene: IV casts the local player as the protagonist, so
// switch to the protagonist's model for it, and put the player back where they were.
bool cutscene_begin() {
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me) return true;
  // Our own character stays behind as a separate ped the player keeps steering (and that
  // the others see); the player ped becomes the cutscene's actor.
  if (!sync::cutscene_double::begin()) return false;
  if (!swap_player_model(episode::protagonist_model())) return false; // "player": streaming, try next frame
  return true;
}
bool cutscene_end() {
  const std::uint32_t model = g_client_mode && !g_restore_pending ? joiner_model_checked(g_original_model) : g_original_model;
  if (model && !swap_player_model(model)) return false;
  const n::Vector3 back = sync::cutscene_double::end(); // where our character walked to
  if (const n::Ped me = n::GetPlayerChar(n::GetPlayerId()); me && (back.x || back.y))
    n::SetCharCoordinatesNoOffset(me, back);
  return true;
}

// While a cutscene plays on a joiner, the host's puppet would double the protagonist (our player
// ped is cast as him), so it is hidden. Partners stay visible where they really are: the
// host's own cutscene has no stand-in for them, and each joiner's stream is their double's.
std::map<std::uint8_t, std::pair<n::Ped, bool>> g_puppet_vis; // last visibility applied, per puppet
void hide_puppets_in_cutscenes() {
  const bool in_cutscene = story::cutscene::playing() && session().status().mode == net::Mode::Joined;
  for (auto& [id, r] : g_remotes) {
    const n::Ped p = r.puppet.ped();
    if (!p || !n::DoesCharExist(p)) continue;
    const bool hide = in_cutscene && id == net::kHostId;
    auto& [last_ped, last_hidden] = g_puppet_vis[id];
    if (last_ped == p && last_hidden == hide) continue;
    if (last_ped != p && !hide) { last_ped = p, last_hidden = false; continue; } // new puppet: visible by default
    last_ped = p, last_hidden = hide;
    n::SetCharVisible(p, !hide);
  }
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
  story::sounds::set_host_ped_provider([] {
    const auto it = g_remotes.find(net::kHostId);
    return it != g_remotes.end() ? it->second.puppet.ped() : 0;
  });
  g_vehicles.interp_delay_ms = s.interp_delay_ms;
  g_vehicles.physics_follow = s.physics_follow;
  g_vehicles.force_type = s.vehicle_force_type;
}

void tick() {
  episode::tick();
  {
    const net::SessionStatus s0 = session().status();
    g_my_role = s0.mode == net::Mode::Idle || s0.mode == net::Mode::Hosting ? net::Role::Coop : s0.my_role;
    // The episode's own protagonist model, measured while nothing of ours has changed the player.
    if (s0.mode == net::Mode::Idle && !g_client_mode && !g_restore_pending && !character::editor_active())
      if (const n::Ped me = n::GetPlayerChar(n::GetPlayerId()); me && n::DoesCharExist(me))
        episode::note_player_model(n::GetCharModel(me));
  }
  handle_hotkeys();
  handle_phone();
  story::sounds::client_tick();
  story::cutscene::client_tick(); // also after leaving, to finish a restore
  sync::cutscene_double::tick();
  story::scene_cam::client_tick(session_settings::current().scenes_watch); // likewise
  hide_puppets_in_cutscenes();
  {
    downed::Frame f;
    const net::SessionStatus st = session().status();
    f.invader = st.mode != net::Mode::Idle && st.my_role == net::Role::Invader;
    f.in_session = st.mode == net::Mode::Hosting || st.mode == net::Mode::Joined;
    f.hosting = st.mode == net::Mode::Hosting;
    f.my_id = st.my_id;
    for (const auto& [id, r] : g_remotes)
      for (const auto& p : st.players)
        if (p.id == id && id != st.my_id && p.role == st.my_role) // invaders and the invaded aren't teammates
          f.teammates.push_back({id, r.name, r.puppet.ped(), r.downed});
    session_settings::tick(f.in_session, f.hosting, session());
    {
      std::string host_name;
      for (const auto& p : st.players)
        if (p.id == net::kHostId) host_name = p.name;
      wallet::tick(st.mode == net::Mode::Joined && !f.invader, host_name); // invaders keep their own money
    }
    {
      character::Frame cf;
      cf.in_session = f.in_session;
      cf.joiner_active = st.mode == net::Mode::Joined && g_client_mode && !g_restore_pending && !f.invader;
      cf.my_id = st.my_id;
      for (const auto& p : st.players) cf.players.push_back(p.id);
      character::tick(cf, session());
    }
    const session_settings::Settings& settings = session_settings::current();
    const bool session_live = f.in_session;
    f.in_session = f.in_session && (settings.downed || f.invader); // off: deaths are normal (invaders are always downed/revived)
    downed::tick(f, session());
    f.in_session = session_live;

    radio_sync::Frame rf;
    rf.in_session = f.in_session && settings.radio_sync && !f.invader, rf.hosting = f.hosting, rf.my_id = f.my_id;
    const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
    if (me && n::DoesCharExist(me) && n::IsCharInAnyCar(me)) {
      const n::Vehicle car = n::StoreCarCharIsInNoSave(me);
      const n::Ped driver = n::GetDriverOfCar(car), front = n::GetCharInCarPassengerSeat(car, 0);
      rf.in_car = true, rf.front_seat = driver == me || front == me;
      for (const auto& [id, r] : g_remotes) {
        if (driver && driver != me && r.puppet.ped() == driver) rf.driver_id = id;
        if (front && front != me && r.puppet.ped() == front) rf.front_id = id;
      }
    }
    radio_sync::tick(rf, session());
    scream::tick(f.in_session, session());
    {
      regroup::Frame gf;
      gf.in_session = session_live && !f.invader, gf.hosting = f.hosting, gf.my_id = f.my_id;
      if (const auto it = g_remotes.find(net::kHostId); it != g_remotes.end() && !f.hosting)
        gf.host_puppet = it->second.puppet.ped();
      regroup::tick(gf, session());
    }
    {
      drunk_sync::Frame df;
      df.in_session = session_live && !f.invader, df.hosting = f.hosting, df.my_id = f.my_id;
      for (const auto& [id, r] : g_remotes)
        if (id != f.my_id) df.mates.push_back({id, r.puppet.ped()});
      if (const auto it = g_remotes.find(net::kHostId); it != g_remotes.end() && !f.hosting)
        df.host_puppet = it->second.puppet.ped();
      drunk_sync::tick(df, session());
    }
    player_mirror::tick(f.hosting && session_live, session());
    race_sync::tick(f.hosting && session_live, session());
    {
      bowling_sync::Frame bf;
      bf.in_session = session_live && !f.invader, bf.hosting = f.hosting, bf.my_id = f.my_id;
      for (const auto& p : st.players) {
        const auto it = g_remotes.find(p.id);
        bf.players.push_back({p.id, p.name, p.id != f.my_id && it != g_remotes.end() ? it->second.puppet.ped() : 0});
      }
      bowling_sync::tick(bf, session());
      activity_seats::tick(bf.in_session, bf.hosting, bf.my_id, session());
      activities::Frame af;
      af.hosting = bf.hosting && bf.in_session, af.my_id = bf.my_id;
      for (const auto& p : bf.players) af.players.push_back({p.id, p.name, p.ped});
      activities::tick(af);
      if (af.hosting) activities::darts::host_send(session()), activities::races::host_send(session());
      activities::darts::client_tick();
      activities::races::client_tick(bf.my_id);
    }
    {
      // Invaders and the invaded fight whatever the Team fire setting says (melee's switch is session-wide).
      bool invaders = f.invader;
      for (const auto& p : st.players) invaders |= p.role == net::Role::Invader;
      melee_sync::tick(f.in_session, f.in_session && (session_settings::damage_allowed() || invaders), session());
    }
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
          attach_sync::host_on_player_joined(net);
          ped_looks::host_on_player_joined(net);
          vehicle_props::host_on_player_joined(net);
          episode_sync::host_on_player_joined(net);
          race_sync::host_on_player_joined(net);
        }
        break;
      case K::Left:
        log::info("Player {} '{}' left", e.id, e.text);
        drop_remote(e.id);
        g_vehicles.drop_owner(e.id);
        g_entities.on_player_left(e.id);
        break;
      case K::Connected:
        log::info("Connected; we are player {} ({})", e.id, e.role == net::Role::Invader ? "invader" : "co-op");
        g_want_client_mode = true;
        episode_sync::on_connected(net);
        if (e.role == net::Role::Invader) {
          const n::Ped p = n::GetPlayerChar(n::GetPlayerId());
          invasion::begin(p && n::DoesCharExist(p) ? p : 0); // where we were, to go back to
        }
        break;
      case K::Disconnected:
        log::info("Disconnected: {}", e.text);
        upnp::close();
        // Tell the player why (version mismatch, session full, host gone, ...); a deliberate
        // leave already said so.
        if (e.text != "left the session") phone::notify("Disconnected: " + e.text);
        drop_all();
        episode_sync::clear();
        race_sync::clear();
        g_vehicles.clear();
        g_entities.clear(g_was_host);
        attach_sync::clear();
        story::clear();
        ped_looks::clear();
        vehicle_props::clear();
        g_mission_cars.clear();
        steam_relay::stop();
        steam::leave_lobby();
        g_invading = false;
        if (g_want_client_mode || g_client_mode) {
          script::set_suppress_predicate(nullptr); // our story scripts resume where they were
          g_want_client_mode = false;
          g_restore_pending = g_client_mode;
        }
        if (invasion::active()) {
          if (g_restore_pending) g_invasion_finish = true; // put things back once our own model is
          else invasion::finish();
        }
        if (e.text.find("invasion") != std::string::npos) phone::notify("That game isn't open to invasion (any more).");
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
  const n::Ped me = local ? sync::capture_ped() : 0;

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
  const auto role_of = [&ss](std::uint8_t id) {
    for (const auto& p : ss.players)
      if (p.id == id) return p.role;
    return net::Role::Coop;
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
  std::vector<invasion::Player> inv_players; // for sync/invasion
  team_fire::tick(); // rounds we fired this frame (hits on player-ped partners)
  for (auto& [id, r] : g_remotes) {
    float dist = 0;
    bool owner_burning = false;
    // Invaders and the invaded are enemies: team fire is always on between them, whatever the setting says.
    const net::Role their_role = role_of(id);
    const bool enemy = invasion::hostile(ss.my_role, their_role);
    invasion::Player ip{id, their_role};
    ip.ped = r.puppet.ped();
    if (auto smp = r.interp.sample(now)) { // once per frame: sampling advances the timeline
      const n::Vector3 at{smp->x, smp->y, smp->z};
      owner_burning = (smp->flags & sync::PlayerState::kOnFire) != 0;
      positions.push_back({id, at});
      ip.known_pos = true, ip.pos = at;
      r.puppet.set_model(smp->model); // their real model (host: Niko); 0 -> configured fallback
      if (r.puppet.ensure(at)) {
        r.puppet.apply_outfit(*smp);
        character::on_puppet(id, r.puppet.ped());
        const bool down = (smp->flags & sync::PlayerState::kDowned) != 0;
        const n::Vehicle veh = g_vehicles.handle_for(smp->vehicle_id);
        ip.ped = r.puppet.ped(), ip.car = veh;
        if (veh) {
          r.pose.up(r.puppet.ped());
          r.puppet.sit_in(veh, smp->seat);
        } else {
          // On foot, or their car hasn't arrived here yet: follow their position on foot.
          if (r.puppet.in_vehicle()) r.puppet.leave_vehicle(at);
          if (down) {
            r.puppet.unlock_ragdoll();
            r.pose.update(r.puppet.ped(), now); // fall, then lie hurt where they are
          } else {
            r.pose.up(r.puppet.ped());
            r.puppet.mirror_body((smp->flags & sync::PlayerState::kRagdoll) != 0,
                                 (smp->flags & sync::PlayerState::kOnFire) != 0, now);
            if (activity_seats::owns(r.puppet.ped())) {
              // seated in the host's activity: its script moves and animates the puppet
            } else if (!melee_sync::playing(r.puppet.ped())) r.puppet.drive(*smp, now);
            else n::SetCharHeading(r.puppet.ped(), smp->heading); // a mirrored melee move runs; keep facing as they do
          }
        }
        if (!down || veh) r.puppet.apply_combat(*smp, now);
        r.puppet.show_health(smp->health);
        r.health = smp->health, r.weapon = smp->weapon, r.dead = (smp->flags & sync::PlayerState::kDead) != 0;
        r.wanted = smp->wanted;
        r.vehicle_id = smp->vehicle_id;
        r.downed = down;
        r.puppet.set_downed_marker(r.downed);
        {
          // Blips: invaders see the target (the host) and their own team; the invaded group sees its partners
          // (the setting) but not the invaders.
          bool blip = session_settings::current().partner_blips;
          if (ss.my_role == net::Role::Invader) blip = their_role == net::Role::Invader || id == net::kHostId;
          else if (their_role == net::Role::Invader) blip = false;
          r.puppet.apply_settings(enemy || session_settings::targeting_allowed(), blip,
                                  enemy || session_settings::damage_allowed());
        }
        {
          // ALLOW_LOCKON_TO_FRIENDLY_PLAYERS switched lock-on to players only (no NPCs) - IV multiplayer's
          // targeting. Keep it off; make sure it is off for anyone who ran the build that set it.
          static bool reset = false;
          if (!reset) n::invoke(0x362B5D1B, n::GetPlayerId(), false), reset = true;
        }
      }
      if (local) dist = std::hypot(smp->x - local->x, smp->y - local->y, smp->z - local->z);
    }
    // Our hits on their puppet go to them; they apply the damage to themselves.
    if (active && me) {
      std::uint16_t dmg = r.puppet.take_hits_from(me, !r.downed); // a downed copy keeps its pose
      std::uint32_t weapon = local ? local->weapon : 0;
      if (r.puppet.is_player_ped() && !r.puppet.engine_damageable()) {
        const team_fire::Hit hit = team_fire::hits_on(r.puppet.ped());
        if (hit.amount) dmg = static_cast<std::uint16_t>(dmg + hit.amount), weapon = hit.weapon;
      }
      if (!dmg && r.puppet.is_player_ped() && team_fire::fire_hit(r.puppet.ped(), owner_burning))
        dmg = team_fire::fire_damage(), weapon = team_fire::kWeaponFire;
      if (dmg && (enemy || session_settings::damage_allowed())) {
        net.send_game(sync::encode_damage({sync::DamageTarget::Player, id, dmg, weapon}), true);
        log::info("Hit player #{} '{}' for {} (weapon {})", id, r.name, dmg, weapon);
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
    ip.dead = r.dead, ip.downed = r.downed;
    inv_players.push_back(ip);
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
    const bool is_invader = ss.my_role == net::Role::Invader;
    for (const auto& m : net.drain_game()) {
      if (!in_session(m.from)) continue;
      if (episode_sync::is_msg(m.data)) {
        episode_sync::on_message(m, ss.mode == net::Mode::Hosting);
        continue;
      }
      if (invasion::is_msg(m.data)) {
        invasion::on_message(m);
        continue;
      }
      // Invaders take no part in the host's story: its objectives, text, blips, payouts, cutscenes, scenes,
      // sounds, radio and regroups are dropped. The world itself (entities, vehicles, players) still arrives.
      if (is_invader && (story::is_story_msg(m.data) || story::sounds::is_msg(m.data) || story::pickups::is_msg(m.data) ||
                         story::scene_cam::is_msg(m.data) || story::fx::is_msg(m.data) ||
                         story::npc_anims::is_msg(m.data) || regroup::is_msg(m.data) || radio_sync::is_msg(m.data) ||
                         drunk_sync::is_msg(m.data) || bowling_sync::is_msg(m.data) ||
                         player_mirror::is_msg(m.data) || race_sync::is_msg(m.data) ||
                         activity_seats::is_msg(m.data) || activities::darts::is_msg(m.data) ||
                         activities::races::is_msg(m.data)))
        continue;
      if (melee_sync::is_msg(m.data)) {
        const auto it = g_remotes.find(m.from);
        melee_sync::on_message(m, ss.my_id, it != g_remotes.end() ? it->second.puppet.ped() : 0);
        continue;
      }
      if (regroup::is_msg(m.data)) {
        regroup::on_message(m);
        continue;
      }
      if (scream::is_msg(m.data)) {
        if (auto it = g_remotes.find(m.from); it != g_remotes.end()) scream::on_message(m, it->second.puppet.ped());
        continue;
      }
      if (drunk_sync::is_msg(m.data)) {
        drunk_sync::on_message(m);
        continue;
      }
      if (player_mirror::is_msg(m.data)) {
        player_mirror::on_message(m);
        continue;
      }
      if (race_sync::is_msg(m.data)) {
        race_sync::on_message(m);
        continue;
      }
      if (activity_seats::is_msg(m.data)) {
        activity_seats::on_message(m);
        continue;
      }
      if (activities::darts::is_msg(m.data)) {
        activities::darts::on_message(m);
        continue;
      }
      if (activities::races::is_msg(m.data)) {
        activities::races::on_message(m);
        continue;
      }
      if (bowling_sync::is_msg(m.data)) {
        bowling_sync::on_message(m);
        continue;
      }
      if (story::sounds::is_msg(m.data)) {
        if (m.from == net::kHostId) story::sounds::client_apply(m.data, g_entities);
        continue;
      }
      if (character::is_msg(m.data)) {
        character::on_message(m);
        continue;
      }
      if (session_settings::is_msg(m.data)) {
        session_settings::on_message(m);
        continue;
      }
      if (story::pickups::is_msg(m.data)) {
        story::pickups::on_message(m, ss.mode == net::Mode::Hosting);
        continue;
      }
      if (vehicle_props::is_msg(m.data)) {
        vehicle_props::on_message(m);
        continue;
      }
      if (radio_sync::is_msg(m.data)) {
        radio_sync::on_message(m);
        continue;
      }
      if (ped_looks::is_msg(m.data)) {
        ped_looks::on_message(m);
        continue;
      }
      if (downed::is_msg(m.data)) {
        downed::on_message(m, ss.my_id);
        continue;
      }
      const auto msg = sync::decode_entity_msg(m.data);
      if (msg && msg->type == sync::GameMsg::Damage) {
        const auto& d = msg->damage;
        if (d.kind == sync::DamageTarget::Player && d.target == ss.my_id && me && !n::IsCharDead(me) &&
            (session_settings::damage_allowed() || invasion::hostile(ss.my_role, role_of(m.from)))) {
          const std::uint32_t h = n::GetCharHealth(me);
          const std::uint32_t left = h > d.amount ? h - d.amount : 0;
          n::SetCharHealth(me, downed::floor_health(left));
          if (d.weapon <= 3) { // a melee hit: stagger
            const auto attacker = g_remotes.find(m.from);
            melee_sync::react_to_melee(attacker != g_remotes.end() ? attacker->second.puppet.ped() : 0);
          }
          if (d.weapon == team_fire::kWeaponFire && !n::invoke<bool>(0x358E21C5, me)) // IS_CHAR_ON_FIRE
            n::invoke(0x5FB31295, me); // START_CHAR_FIRE: we burn from here on (the engine does the rest)
          if (d.weapon == team_fire::kWeaponVehicle || d.weapon == team_fire::kWeaponExplosion)
            n::invoke(0x1A0F56C5, me, 0, 2000, 1, 1, 1, 0); // knocked over (SWITCH_PED_TO_RAGDOLL)
          log::info("Took {} damage from player #{} (weapon {}), health {} -> {}", d.amount, m.from, d.weapon, h,
                    h > d.amount ? h - d.amount : 0);
        } else if (d.kind == sync::DamageTarget::Entity) {
          g_entities.apply_damage(d.target, d.amount, ss.my_id);
        }
        continue;
      }
      if (attach_sync::is_msg(m.data)) {
        attach_sync::on_message(m, g_entities);
        continue;
      }
      if (story::scene_cam::is_msg(m.data)) {
        if (m.from == net::kHostId) story::scene_cam::client_apply(m.data);
        continue;
      }
      if (story::fx::is_msg(m.data)) {
        story::fx::client_on_message(m);
        continue;
      }
      if (story::npc_anims::is_msg(m.data)) {
        if (m.from == net::kHostId) story::npc_anims::client_apply(m.data, g_entities);
        continue;
      }
      if (story::client_on_message(m, g_entities)) continue; // story text/blips from the host
      g_entities.on_message(m, ss.my_id);
    }
    // Story (M6): the host sends what its story scripts did this frame; clients retry
    // blips whose entity copy wasn't there yet.
    story::host_tick(g_was_host, net, g_entities);
    ped_looks::host_tick(g_was_host, net, g_entities);
    if (!g_was_host) {
      const auto host_remote = g_remotes.find(net::kHostId);
      if (!is_invader) story::client_tick(g_entities, host_remote != g_remotes.end() ? host_remote->second.puppet.ped() : 0);
      ped_looks::client_tick(g_entities);
      if (!is_invader) handle_mission_cars(ss.my_id);
    }
    {
      // Invasions: the host's window; an invader's spawn and hunt.
      invasion::Frame inv;
      inv.in_session = true, inv.hosting = g_was_host, inv.my_id = ss.my_id, inv.my_role = ss.my_role;
      inv.ready = is_invader && g_client_mode && !g_restore_pending;
      for (const auto& p : ss.players)
        if (p.id == net::kHostId) inv.host_name = p.name;
      inv.players = std::move(inv_players);
      invasion::tick(inv, net);
      if (auto why = invasion::take_leave()) {
        log::info("Leaving the invasion: {}", *why);
        net.leave();
        steam_relay::stop();
        steam::leave_lobby();
        phone::notify(*why);
      }
      // The Steam lobby advertises the window (a lobby made after it opened is caught up here).
      if (g_was_host && steam::available() && invasion::open() != g_lobby_flag) {
        g_lobby_flag = invasion::open();
        steam::set_lobby_open(g_lobby_flag);
      }
    }
    g_entities.player_ped = [my_id = ss.my_id](std::uint8_t id) -> n::Ped {
      if (id == my_id) return n::GetPlayerChar(n::GetPlayerId());
      const auto it = g_remotes.find(id);
      return it != g_remotes.end() ? it->second.puppet.ped() : 0;
    };
    attach_sync::host_tick(g_was_host, net, g_entities);
    if (!g_was_host) attach_sync::client_tick(g_entities);
    sync::ambient::tick(now, true, g_was_host, session_settings::current().ambient_sync, ss.my_id, positions, g_entities, net);
    g_entities.tick(now, ss.my_id, g_was_host, positions, net);
    vehicle_props::tick(true, g_was_host, g_entities, net);
    for (const auto& hit : g_entities.collect_hits(me)) {
      net.send_game(sync::encode_damage({hit.kind, hit.target, hit.amount, local ? local->weapon : 0}), true);
      log::info("Hit entity {} for {}", hit.target, hit.amount);
    }
  }
  if (!active) {
    invasion::Frame none;
    invasion::tick(none, net); // closes the window
    g_lobby_flag = false;
  }
  if (!active) story::host_tick(false, net, g_entities), ped_looks::host_tick(false, net, g_entities); // no session: capture stays off
  if (!active) vehicle_props::tick(false, false, g_entities, net);
  if (!active) sync::ambient::tick(now, false, false, false, 0, {}, g_entities, net); // restores density
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

bool host() {
  if (session().status().mode != net::Mode::Idle) return session().status().mode == net::Mode::Hosting;
  if (!session().host(g_cfg.port, g_cfg.name, g_cfg.bind_address)) {
    log::error("Hosting failed: {}", session().status().last_error);
    return false;
  }
  log::info("Hosting on UDP {} as '{}' (mission runner)", g_cfg.port, g_cfg.name);
  return true;
}

bool join(const std::string& address) {
  if (session().status().mode != net::Mode::Idle) return false;
  if (!session().join(address, g_cfg.port, g_cfg.name)) {
    log::error("Joining failed: {}", session().status().last_error);
    return false;
  }
  log::info("Joining {}:{} as '{}' (self-test)", address, g_cfg.port, g_cfg.name);
  return true;
}

natives::Ped puppet_ped(std::uint8_t id) {
  const auto it = g_remotes.find(id);
  return it != g_remotes.end() ? it->second.puppet.ped() : 0;
}

natives::Vehicle puppet_vehicle(std::uint8_t id) {
  const auto it = g_remotes.find(id);
  if (it == g_remotes.end() || it->second.vehicle_id == sync::kNoVehicle) return 0;
  return g_vehicles.handle_for(it->second.vehicle_id);
}

void leave() {
  if (session().status().mode == net::Mode::Idle) return;
  log::info("Leaving session (self-test)");
  session().leave();
}

bool start_recording(const std::string& path) {
  if (session().recording()) session().stop_recording();
  if (!session().start_recording(path)) return false;
  log::info("Recording session to {}", path);
  g_record_started_ms = net::steady_ms();
  return true;
}

void stop_recording() {
  if (session().recording()) session().stop_recording(), log::info("Recording stopped");
}

} // namespace ivc::coop
