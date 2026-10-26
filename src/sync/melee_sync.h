#pragma once
// Melee for player partners: moves are mirrored, and the one being hit decides the outcome.
//
// Mirroring: the game lists every melee animation in common/data/MeleeAnims.dat (group, dictionary,
// clips: attacks like Jab / Cross_R / Hook_L, Block, hit and block reactions, counters). Each game
// checks which of those clips its own player is playing and sends each new one; partners' copies play
// the same clip (TASK_PLAY_ANIM), pausing their walk-to-position while it runs.
//
// Hits (sync/team_fire detects our strikes): the victim's game decides. If its player is blocking (the
// Block clip or a block reaction is playing) the hit does no damage and they play a block reaction;
// otherwise they take the damage and play the hit reaction for the side it came from. Those reactions are
// clips too, so the attacker sees them through the mirroring.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "game/natives.h"
#include "net/session.h"

namespace ivc::melee_sync {

constexpr std::uint8_t kMsg = 46;

// Reads MeleeAnims.dat (game folder: common/data). Once, at startup.
void load(const std::filesystem::path& game_dir);
// Game thread, every frame: finds the clip our player is playing and sends new ones.
void tick(bool in_session, net::Session& net);
inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }
// A partner started a melee clip: their copy (`ped`) plays it.
void on_message(const net::GameMessage& m, natives::Ped ped, std::uint32_t now_ms);
// True while a copy is playing a mirrored clip (its driver should leave it alone).
bool playing(natives::Ped ped);

// Victim side, for a melee hit from `attacker` (their copy here): true if we blocked it (no damage). Plays
// our block or hit reaction either way.
bool react_to_hit(natives::Ped attacker);

} // namespace ivc::melee_sync
