#pragma once
// Changing the player's model safely.
//
// The TBoGT join crash (0.9 betas): client mode's swap from Luis to the co-op character hid the player first
// (SET_CHAR_VISIBLE false). A ped changed while hidden kept its facial blend shapes (rage::crCreatureComponentBlendShapes)
// registered in the render-callback list after it was freed, so the render thread called into freed memory (execute
// of 0x437F0000) and then the main thread faulted at 0x6A1ADB. 0.8 swapped a visible player and didn't crash; so does
// this. A swap still waits until the player hasn't jumped for a moment, and busy() keeps our teleports away from it.
//
// install() also guards two engine paths that read a just-changed or freed ped (cheap, and logged when they fire):
// the ped add-to-draw-list (0xC64520) skips a ped whose render data (CPed+0x310) isn't built yet (the 0x370-byte copy
// from null at 0xDF969A), and the render thread's object callback (0x8DDE30) skips an object whose vtable isn't in
// the game's image.

#include <cstdint>

#include "game/natives.h"

namespace ivc::model_swap {

// Once, at startup: the guards above.
void install();
// Game thread. false: not yet (keep calling each frame); true: the player now wears `model` (it must be loaded).
bool change_player_model(natives::Player player, std::uint32_t model);
// Game thread, every frame: notices the player jumping; logs what the guards skipped.
void tick();
// A swap is waiting or just happened: don't teleport the player now.
bool busy();

} // namespace ivc::model_swap
