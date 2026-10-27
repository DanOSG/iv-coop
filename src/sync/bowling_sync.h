#pragma once
// Multiplayer tenpin bowling (first slice). The host's TENPINBOWL script stays the game: it owns the lane's
// ball and pins, the throw physics, the cameras and the animations. Partners take turns on the same lane by
// *driving the host's script*: while it is a partner's turn the partner's stick/mouse/button state is sent to
// the host (message 59, ~30 Hz), which answers the script's own input reads (GET_POSITION_OF_ANALOGUE_STICKS,
// GET_MOUSE_INPUT, IS_CONTROL_PRESSED / IS_CONTROL_JUST_PRESSED, IS_USING_CONTROLLER) with the partner's values
// instead of the host's. The host's character does the run-up and throw; the partner aims and releases.
//
// Turn order: the host, then each partner within kPresentRange of the host's player when the game starts,
// rotating per frame (a strike, or the second ball, ends a frame; the 10th frame has fill balls). The
// script's own scorecard has one card, so the mod keeps its own: it watches the lane (the pins the script
// creates, the ball's release, which pins are down by the script's own test: moved over 0.2 m or tilted over
// 15 degrees) and scores each player's rolls with standard bowling rules. The scorecard is drawn as an
// overlay on every machine with each player's name, frames and score.
//
// The game is the script's 10 frames shared by all the players (so 2 players get 5 frames each). Not yet:
// a full 10 frames each (needs the script's frame counter held, docs/ARCHITECTURE.md), a camera for the
// partner, and bowling with a friend (AI bowler) in the party.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/natives.h"
#include "net/session.h"

namespace ivc::bowling_sync {

constexpr std::uint8_t kMsg = 59;
constexpr float kPresentRange = 20.0f; // metres from the host's player when the game starts

struct Player {
  std::uint8_t id;
  std::string name;
  natives::Ped ped; // the partner's copy on this machine (host side: used for the range check); 0 for us
};

struct Frame {
  bool in_session = false, hosting = false;
  std::uint8_t my_id = 0;
  std::vector<Player> players; // everyone in the session, including us
};

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Register before native_hooks::install().
void register_hooks();
// Game thread, every frame, from a script context (draws the scorecard).
void tick(const Frame& f, net::Session& net);
void on_message(const net::GameMessage& m);

// Scoring (exposed for tests): one frame's rolls.
struct Roll {
  std::uint8_t r[3] = {0, 0, 0};
  std::uint8_t n = 0;
  std::uint8_t last = 0; // the 10th frame (fill balls)
};
// Running score of a player's frames, bonuses counted as far as their later rolls exist.
int score(const std::vector<Roll>& frames);

} // namespace ivc::bowling_sync
