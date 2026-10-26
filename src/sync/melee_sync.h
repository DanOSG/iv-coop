#pragma once
// Melee between players, built on the game's own melee data.
//
// Mirroring: common/data/MeleeAnims.dat lists every melee clip (group -> dictionary, clips). Each game
// finds the melee clip its player is playing - attacks, Block, hit and block reactions, counters; not the
// melee locomotion set, which plays underneath everything - and sends each new one; partners' copies play
// it (fast blend), facing the way the player faces.
//
// Contact: common/data/action_table.csv gives each attack (its "_Res" row) its striking bones and radii
// (a jab: L_HAND 0.05 m, L_FOREARM 0.06 m), damage, blocked-recoil and impact sound. While our player
// swings an attack, its striking bones are tested every frame against each partner copy's head, neck and
// chest; the first touch is the hit (once per swing) - so it lands when the blow actually connects, and a
// step back or a dodge avoids it. The hit goes to the victim's game, which decides:
//   - blocking (the Block clip or a block reaction) and facing it: no damage, they play the block
//     reaction for that side, and we're told - our player plays the attack's recoil (the fist bouncing
//     off);
//   - otherwise the damage (table value x kDamageScale) and the hit reaction for that side.
// The impact sound plays on both screens. Reactions on the victim play as an overlay (they keep control,
// like the engine's flinch).

#include <cstdint>
#include <filesystem>
#include <span>

#include "game/natives.h"
#include "net/session.h"

namespace ivc::melee_sync {

constexpr std::uint8_t kMsgClip = 46, kMsgHit = 47, kMsgBlocked = 48;

// Reads MeleeAnims.dat and action_table.csv (game folder: common/data). Once, at startup.
void load(const std::filesystem::path& game_dir);

struct Partner {
  std::uint8_t id;
  natives::Ped ped; // their copy here
};
// Game thread, every frame: our clip (mirroring) and our swings' contacts with `partners`. `can_hit`: the
// session's team fire allows damage.
void tick(bool in_session, bool can_hit, std::span<const Partner> partners, net::Session& net);

inline bool is_msg(std::span<const std::uint8_t> d) {
  return !d.empty() && (d[0] == kMsgClip || d[0] == kMsgHit || d[0] == kMsgBlocked);
}
// `from_ped`: the sender's copy here. Applies hits to us (damage floored by downed), plays reactions.
void on_message(const net::GameMessage& m, std::uint8_t my_id, natives::Ped from_ped, net::Session& net);
// True while a copy is playing a mirrored clip (its driver should leave it alone).
bool playing(natives::Ped ped);

} // namespace ivc::melee_sync
