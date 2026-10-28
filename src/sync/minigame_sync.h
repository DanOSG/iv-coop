#pragma once
// Multiplayer pool and darts, in all three games (the scripts "pool_game" and "darts" are the same in each). As with
// bowling (sync/bowling_sync), the host's minigame script stays the game: it owns the table or board, the physics,
// the cameras, the score and the AI friend. Everyone at the table plays the human side as a team against the friend,
// taking turns: on a partner's turn the partner's sticks, mouse and buttons are sent to the host (message 73,
// ~30 Hz) and the host answers the script's input reads with them instead of its own.
//
// Whose go it is: the script reads the aim input (sticks / mouse) only while the human side is lining up a shot or a
// dart. A pause of kTurnGapMs without those reads (the ball rolling, the dart flying, the friend's turn) ends a
// go; the next read starts the next player's go. So pool plays as alternate shots ("scotch doubles") and darts as
// alternate darts. The players are the host and every partner within kPresentRange of the host when the script
// starts (in session order). Partners watch through the host's camera (story/scene_cam) and see the score and
// boards (story/hud), and their own character stands still on their go.
//
//   [73][sender][op][...]
//   op 0, host -> everyone (reliable, on change): active u8, script u8 (0 pool, 1 darts), controller id u8,
//         players u8 (bit per session id at the table)
//   op 1, partner -> host (~30 Hz, reliable when a button edge is in it): controls u16, just u16, buttons u32,
//         mouse buttons u8, controller u8, sticks lx ly rx ry i16, mouse x y i16, keyboard move x y i16

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/natives.h"
#include "net/session.h"

namespace ivc::minigame_sync {

constexpr std::uint8_t kMsg = 73;
constexpr float kPresentRange = 15.0f;

struct Player {
  std::uint8_t id;
  std::string name;
  natives::Ped ped; // the partner's copy here (host: the range check); 0 for us
};

struct Frame {
  bool in_session = false, hosting = false;
  std::uint8_t my_id = 0;
  std::vector<Player> players; // everyone in the session, including us
};

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Before native_hooks::install().
void register_hooks();
// Game thread, every frame.
void tick(const Frame& f, net::Session& net);
void on_message(const net::GameMessage& m);

// Host: a game with partners is on (story/scene_cam streams the table camera only then).
bool cam_wanted();
// Joiner: we're one of the players at the host's table (we watch the table camera).
bool participating();

} // namespace ivc::minigame_sync
