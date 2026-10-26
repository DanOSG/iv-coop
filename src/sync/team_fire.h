#pragma once
// Our shots on partners who are player peds.
//
// Partners are extra player peds (game/player_slots), and the single-player engine never applies
// player-on-player damage: WeaponInfo.xml's per-weapon "networkplayermod" shows that path exists only in
// network games. So we detect our own hits: when a round leaves our gun (ammo drops) while we're locked
// on to or free-aiming at a partner within the weapon's range, that partner is hit for the weapon's
// player-vs-player damage, base x networkplayermod - Rockstar's own multiplayer numbers, read from the
// game's common/data/WeaponInfo.xml. The victim's game applies it (victim-owner damage, as for all hits).
// The same goes for our melee strikes (locked on within reach), our car hitting them at speed and our
// grenades/rockets going off near them; those knock the victim down in their game too.
// Not covered yet: molotov/fire damage.

#include <cstdint>
#include <filesystem>

#include "game/natives.h"

namespace ivc::team_fire {

// Reads WeaponInfo.xml (game folder: common/data). Once, at startup.
void load(const std::filesystem::path& game_dir);
// Game thread, once per frame before hits_on(): counts the rounds we fired this frame.
void tick();
constexpr std::uint8_t kWeaponExplosion = 253, kWeaponVehicle = 254; // Damage.weapon for non-weapon hits

struct Hit {
  std::uint16_t amount = 0;
  std::uint8_t weapon = 0;
  bool knockdown = false; // the victim ragdolls (car, explosion)
};
// What we did to `target` this frame (amount 0: nothing).
Hit hits_on(natives::Ped target);

} // namespace ivc::team_fire
