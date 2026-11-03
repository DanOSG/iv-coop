#pragma once
// Player mirror: what the host's story does to the host's own player is done to every partner's player too, in all
// three games. Missions hand out and take away weapons (the episodes' parachute is weapon 41, given before every
// skydive), add armour or a vest, set health, and make the player unable to jump/crouch for
// stealth sections; partners following along need the same to keep up.
//
//   [69][sender][op][a u32][b u32]   (reliable, host -> everyone)
//   op 0 give weapon a with b ammo · 1 remove weapon a · 2 remove all weapons · 3 add armour a · 4 vest a (0/1) ·
//   5 set health a · 6 (unused: SET_PLAYER_INVISIBLE_TO_AI is a no-op here) · 7 jump disabled a · 8 crouch disabled a
//
// Only calls from story scripts on the host's player count (shops and other local scripts run on every machine and
// pay for themselves). Invaders get none of it.

#include <cstdint>
#include <span>

#include "net/session.h"

namespace ivc::player_mirror {

constexpr std::uint8_t kMsg = 69;

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Before native_hooks::install().
void register_hooks();
// Game thread, every frame: sends what the host's story did since the last call.
// `ready` (joiners): our character is the one we'll play the session as (client mode has swapped the model; a
// swap drops weapons, so ops wait for it).
void tick(bool hosting, net::Session& net, bool ready = true);
// Joiner: apply to our own player.
void on_message(const net::GameMessage& m);

} // namespace ivc::player_mirror
