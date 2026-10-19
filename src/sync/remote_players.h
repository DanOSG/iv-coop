#pragma once
// In-game side of the session: sends our state, and gives every other player in the
// session a puppet ped driven by their interpolated state. Hotkeys start/stop hosting
// and joining until there is a proper menu.

#include <cstdint>
#include <string>
#include <vector>

#include "net/session.h"
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
  int host_key = 0x79; // VK_F10: host / leave
  int join_key = 0x7A; // VK_F11: join / leave
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
};

struct Status {
  net::SessionStatus session;
  std::vector<RemoteStatus> remotes;
  std::vector<sync::RemoteVehicleStatus> vehicles;
};

void configure(const Settings& s);
// Game thread, script context.
void tick();
// Any thread.
Status status();

} // namespace ivc::coop
