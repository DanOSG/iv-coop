#pragma once
// Extra *player* peds (player slots 1..7) for remote players. Only player peds may wear the
// protagonist model ("player": Niko, Johnny, Luis; CREATE_CHAR with it crashes), and player peds
// react to hits like a player (no NPC knockdowns), so every remote player gets one while slots last. The script native CREATE_PLAYER only (re)creates the local
// player, so this calls the engine function behind it with a slot of our own, the way the
// native netcode creates remote players. Engine internals by static address
// (GTAIV.exe 1.2.0.59); see the .cpp for each one and why.
//
// Slots are never freed: the engine only frees player slots safely at session teardown
// (freeing one mid-game crashed on a queued job still referencing the ped). A released
// slot is parked hidden and frozen, and the next acquire reuses it.

#include <vector>

#include "game/natives.h"

namespace ivc::player_slots {

constexpr std::uint32_t kProtagonistModel = 0x6F0783F5; // joaat("player")

// Game thread, script context. A player ped wearing `model` (loaded; CHANGE_PLAYER_MODEL) at `at`, ready
// to be driven with tasks (no input, ignored by police and peds), or 0 on failure / all slots in use.
natives::Ped acquire(const natives::Vector3& at, float heading, std::uint32_t model = kProtagonistModel);
// Parks a ped returned by acquire. Anything else is ignored.
void release(natives::Ped ped);
bool owns(natives::Ped ped);
// The engine's can-be-damaged check (0xCA3360) refuses all damage to a player ped whose CPlayerInfo lacks
// flag 0x10 at +0x70+0x3D0; the local player has it, our slots are created without it. Setting it lets the
// engine damage a partner's copy natively - melee (contact, blocks, reactions, sounds), bullets, cars,
// explosions - the way IV multiplayer damages remote players. Returns the flag's previous state.
bool set_damageable(natives::Ped ped, bool damageable);
// The script handles of the slots' peds in use (a partner's character each).
std::vector<natives::Ped> peds_in_use();
// The CPed* is one of our slots' (for the health hooks).
bool is_slot_object(const void* ped_object);

} // namespace ivc::player_slots
