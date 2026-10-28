#pragma once
// Race sync: the checkpoints and GPS race track the host's story scripts put up (street races, TLAD's bike races,
// TBoGT's races and base jumps, chase objectives) appear for every partner too. Markers drawn each frame
// (DRAW_CHECKPOINT) go with the HUD (story/hud); this is for the persistent ones.
//
//   [71][sender][op][...]   (reliable, host -> everyone)
//   op 0 create checkpoint: id u32, type i32, x y z f32, a5 a6 f32 · op 1 delete checkpoint: id u32 ·
//   op 2 start GPS race track: colour i32 (clears its points) · op 3 track point: x y z f32 ·
//   op 4 render track: on u8 · op 5 loop track: on u8 · op 6 clear everything (sent before a full resend)
//
// A partner who joins mid-race gets the whole state. Invaders get none of it.

#include <cstdint>
#include <span>

#include "net/session.h"

namespace ivc::race_sync {

constexpr std::uint8_t kMsg = 71;

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Before native_hooks::install().
void register_hooks();
// Game thread, every frame.
void tick(bool hosting, net::Session& net);
void host_on_player_joined(net::Session& net);
void on_message(const net::GameMessage& m);
// Joiner: the session ended; take down what we put up.
void clear();
// Joiner: checkpoints we put up for the host (self-test).
std::size_t local_count();

} // namespace ivc::race_sync
