#pragma once
// Changing a player ped's model safely. CHANGE_PLAYER_MODEL rebuilds the per-ped render data (CPed+0x310) of the other
// player peds wearing that model too, while the render thread is still drawing the previous frame's list: a partner's
// puppet in view when we swapped to the same co-op character left a draw command calling into freed memory (execute
// of 0x437F0000 on the render thread), and drawing it in the swap's own frame copied from null (static VA 0xDF969A,
// the 0x370-byte copy from 0x8DB083). Both crashed the game now and then on joining. So every player-model change
// first holds all player peds (ours and the player slots) out of the draw list for a few frames (hold_player_peds),
// keeps them out a few frames after, and install()'s guard on the ped add-to-draw-list (0xC64520) also skips any ped
// whose render data isn't built. A swap of our own player also hides it a moment either side and waits until it
// hasn't jumped for a moment. Callers already retry a change that isn't done yet.

#include <cstdint>

#include "game/natives.h"

namespace ivc::model_swap {

// Once, at startup: the draw guard above.
void install();
// Game thread, each frame until true, right before changing any player ped's model (CHANGE_PLAYER_MODEL, or creating a
// player slot): all player peds are out of the draw list now; they stay out a few frames after the last call.
bool hold_player_peds();
// Game thread. false: not yet (keep calling each frame); true: the player now wears `model` (it must be loaded).
bool change_player_model(natives::Player player, std::uint32_t model);
// Game thread, every frame: ends the hold and shows the player again once a swap has settled; notices jumps.
void tick();
// A swap is under way, just happened, or is expected (expect_change): don't teleport the player now, and don't take a
// player slot. In TBoGT, a partner's puppet wearing the co-op character near us when we changed to it too was left
// with dangling facial blend-shape data (rage::crCreatureComponentBlendShapes): the render callbacks above, then
// 0x6A1ADB on the main thread, read freed memory. So on joining, puppets spawn only after our own change.
bool busy();
void expect_change(bool expected);

} // namespace ivc::model_swap
