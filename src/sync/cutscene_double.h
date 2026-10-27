#pragma once
// Joiner's free body during a replicated cutscene (M7).
//
// IV casts the local player ped as the cutscene's protagonist (no native can exclude or
// substitute it), so for the replay the player ped is the scripted actor. The joiner's own
// character is a separate "double" ped created where they stood when the cutscene began;
// the player steers it with the keyboard/pad (relative to the cutscene camera) and it is
// what the other players see and what the player layer streams (sync::capture_local) while
// the cutscene plays. At the end the real player ped is put where the double stands.

#include "game/natives.h"

namespace ivc::sync::cutscene_double {

// Creates the double at the local player's position; true once it exists (it may need a few
// frames to stream the model). Does nothing if one exists.
bool begin();
// Per frame: steers the double while it exists.
void tick();
// Removes the double; returns where it stood (the player's place afterwards), or the
// player's own position if there was none.
natives::Vector3 end();
// The ped that represents the local player to the network: the double while it exists.
natives::Ped ped();
bool active();

} // namespace ivc::sync::cutscene_double
