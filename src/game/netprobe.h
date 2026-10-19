#pragma once
// Experiment: can GTA IV's own (native) network session come up inside a story save?
// See docs/research-native-netcode.md section 7. Black-box only: it calls the game's
// network natives and reads plain game-state globals; it never patches anything.
//
// Disabled unless [NetProbe] Enabled=1. While enabled:
//   - logs the native network state whenever it changes
//   - Ctrl+Shift+F12: NETWORK_SET_LAN_SESSION(1) + NETWORK_HOST_GAME(HostArgs), then
//     follows the attempt frame by frame (pending/succeeded, game mode, script count)
//   - Ctrl+Shift+F11: NETWORK_LEAVE_GAME
// Use a throwaway save: a game-mode switch may reset or autosave the world.

#include <array>
#include <cstdint>

namespace ivc::netprobe {

struct Settings {
  bool enabled = false;
  // NETWORK_HOST_GAME(game_mode, ranked, max_players, flag, episode, teams). The meaning
  // of each value comes from IV's MP scripts; the game-mode id in particular is a guess.
  std::array<std::int32_t, 6> host_args{16, 0, 4, 0, 0, 0};
};

void configure(const Settings& s);
// Game thread, script context.
void tick();

} // namespace ivc::netprobe
