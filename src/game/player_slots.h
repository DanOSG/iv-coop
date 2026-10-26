#pragma once
// Extra *player* peds (player slots 1..3) for remote players. Only player peds may wear the
// protagonist model ("player": Niko, Johnny, Luis; CREATE_CHAR with it crashes), and player peds
// react to hits like a player (no NPC knockdowns), so every remote player gets one while slots last. The script native CREATE_PLAYER only (re)creates the local
// player, so this calls the engine function behind it with a slot of our own, the way the
// native netcode creates remote players. Engine internals by static address
// (GTAIV.exe 1.2.0.59); see the .cpp for each one and why.
//
// Slots are never freed: the engine only frees player slots safely at session teardown
// (freeing one mid-game crashed on a queued job still referencing the ped). A released
// slot is parked hidden and frozen, and the next acquire reuses it.

#include "game/natives.h"

namespace ivc::player_slots {

constexpr std::uint32_t kProtagonistModel = 0x6F0783F5; // joaat("player")

// Game thread, script context. A player ped wearing `model` (loaded; CHANGE_PLAYER_MODEL) at `at`, ready
// to be driven with tasks (no input, ignored by police and peds), or 0 on failure / all slots in use.
natives::Ped acquire(const natives::Vector3& at, float heading, std::uint32_t model = kProtagonistModel);
// Parks a ped returned by acquire. Anything else is ignored.
void release(natives::Ped ped);
bool owns(natives::Ped ped);

} // namespace ivc::player_slots
