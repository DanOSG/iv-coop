#pragma once
// In-game side of the session: sends our state, and gives every other player in the
// session a puppet ped driven by their interpolated state. Hotkeys start/stop hosting
// and joining until there is a proper menu.

#include <cstdint>
#include <string>
#include <vector>

#include "net/session.h"
#include "sync/entity_sync.h"
#include "sync/vehicle_sync.h"

namespace ivc::coop {

struct Settings {
  std::string name = "Player";
  std::string join_address = "127.0.0.1";
  std::uint16_t port = 41300;
  std::string bind_address = "0.0.0.0";
  std::uint32_t send_hz = 30;
  std::uint32_t interp_delay_ms = 100;
  std::string puppet_model = "M_Y_MULTIPLAYER";
  bool animated_puppets = true;
  bool physics_follow = true;       // stand-in cars react to traffic/walls (vs. kinematic)
  // Client mode (M6d): while joined, play as this character (not the story protagonist) and
  // freeze our own story scripts (the host runs the story). Both undone on leaving.
  std::string joiner_model = "M_Y_MULTIPLAYER";
  bool freeze_story_scripts = true;
  std::uint32_t vehicle_force_type = 1;
  int host_key = 0x79; // VK_F10: host / leave
  int join_key = 0x7A; // VK_F11: join / leave
  int invade_key = 0x78; // VK_F9: join join_address as an invader / leave (dev path; the phone's Invade uses Steam)
};

struct RemoteStatus {
  std::uint8_t id;
  std::string name;
  std::uint32_t rtt_ms;
  bool spawned;
  float extrapolated_pct;
  float distance_m; // from the local player
  const char* action; // what the puppet is doing (walking, standing, snapped, ...)
  float error_m;      // puppet's distance from the true (interpolated) position
  std::uint32_t snaps;
  std::uint16_t health;
  std::uint32_t weapon;
  bool dead;
  std::uint8_t wanted;
};

struct Status {
  net::SessionStatus session;
  std::vector<RemoteStatus> remotes;
  std::vector<sync::RemoteVehicleStatus> vehicles;
  std::vector<sync::EntityStatus> entities;
  bool recording = false;
  std::uint32_t recording_ms = 0;
  unsigned story_messages = 0;
};

void configure(const Settings& s);
// Game thread, script context.
void tick();
// Any thread.
Status status();

// For the unattended mission runner (game thread): host on the configured port, as F10 does,
// and record everything we send, as Ctrl+Shift+R does.
bool host();
bool start_recording(const std::string& path);
// Self-test (game thread): join `address` on the configured port as F11 does, and leave whatever session we're in.
bool join(const std::string& address);
void leave();
// The puppet ped of player `id` here (0 if none).
natives::Ped puppet_ped(std::uint8_t id);
void stop_recording();

} // namespace ivc::coop
