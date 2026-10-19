#pragma once
#include <cstdint>
#include <string>

namespace ivc::ghost {

// M1 sync test: the local player's state goes through the real encoder, a loopback
// transport with artificial latency, and the interpolator, then drives a spawned ped
// standing next to you. If the ghost moves smoothly ~latency behind you, the pipeline works.

struct Settings {
  std::string model = "M_Y_MULTIPLAYER";
  int toggle_key = 0x78;        // VK_F9
  std::uint32_t send_hz = 30;
  std::uint32_t latency_ms = 200;
  std::uint32_t jitter_ms = 30;
  float loss = 0.05f;
  std::uint32_t interp_delay_ms = 100;
  float offset_x = 2.0f;        // world-space offset so the ghost doesn't overlap you
};

struct Status {
  bool enabled = false;
  bool spawned = false;
  std::string phase;            // human-readable state for the overlay
  std::uint32_t sent = 0, received = 0, buffered = 0, dropped = 0;
  float player_x = 0, player_y = 0, player_z = 0, player_heading = 0;
};

void configure(const Settings& s);
// Game thread, inside script context.
void tick();
// Any thread.
Status status();

} // namespace ivc::ghost
