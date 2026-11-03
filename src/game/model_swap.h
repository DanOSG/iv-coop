#pragma once
// Changing the player's model without the engine drawing it mid-change. CHANGE_PLAYER_MODEL on a visible player crashed
// the renderer now and then (a CDrawPedDC for the player failed to allocate the frame after a swap: seen on joining,
// when client mode swaps to the co-op character, in all three games). The player is hidden for a few frames first,
// swapped, and shown again a few frames later; callers already retry a swap that isn't done yet.

#include <cstdint>

#include "game/natives.h"

namespace ivc::model_swap {

// Game thread. false: not yet (keep calling each frame); true: the player now wears `model` (it must be loaded).
bool change_player_model(natives::Player player, std::uint32_t model);
// Game thread, every frame: shows the player again once a swap has settled.
void tick();

} // namespace ivc::model_swap
