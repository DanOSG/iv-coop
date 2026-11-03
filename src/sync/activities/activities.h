#pragma once
// Multiplayer activities: each player plays as their own character with their own score and turns. One adapter per
// activity script family, all built on sync/activity_seats (seat a partner's puppet in the host's script, relay their
// input, make the script's GET_PLAYER_CHAR return them on their turn, protect the puppet from the script's cleanup).
// The host's activity script stays the game; adapters only decide who sits where and whose turn it is, and write the
// script's own seat/score statics where the script has fewer seats than players.
//
// Adapters (src/sync/activities/<name>.cpp, each with register_hooks() and host_tick(const Frame&)):
//   pool          1v1: a partner takes the opponent seat; others queue and challenge the winner
//   darts         up to 4: each player's 301 rotated through the script's human seat; scoreboard drawn by the mod
//   golf          TBoGT, up to 4: each player takes the player path with their own scorebook
//   tlad_games    TLAD arm wrestling (1v1 via the script's own second-pad mode) and hi-lo cards (4 seats)
//   air_hockey    TLAD / TBoGT 1v1: a partner takes the far seat, paddle 1 driven from their input
// Bowling is sync/bowling_sync (up to 4, each their own bowler). docs/activities/ has the research per script.

#include <cstdint>
#include <string>
#include <vector>

#include "game/natives.h"

namespace ivc::activities {

struct Player {
  std::uint8_t id;
  std::string name;
  natives::Ped ped; // the partner's puppet here; 0 for us
};

struct Frame {
  bool hosting = false; // in a session, as its host (adapters act only then)
  std::uint8_t my_id = 0;
  std::vector<Player> players; // everyone in the session, including us
};

// Partners within this distance of the host's player when an activity starts take part.
constexpr float kPresentRange = 15.0f;

// Before native_hooks::install().
void register_hooks();
// Game thread, every frame (script context).
void tick(const Frame& f);

// Shared helpers for the adapters.
natives::Ped host_ped();
// Partners near the host's player now, in session order.
std::vector<const Player*> partners_near(const Frame& f, float range = kPresentRange);

} // namespace ivc::activities
