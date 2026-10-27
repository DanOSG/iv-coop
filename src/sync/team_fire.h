#pragma once
// Our shots on partners who are player peds.
//
// Partners are extra player peds (game/player_slots), and the single-player engine never applies
// player-on-player damage: WeaponInfo.xml's per-weapon "networkplayermod" shows that path exists only in
// network games. So we detect our own hits: when a round leaves our gun (ammo drops) while we're locked
// on to or free-aiming at a partner within the weapon's range, that partner is hit for the weapon's
// player-vs-player damage, base x networkplayermod - Rockstar's own multiplayer numbers, read from the
// game's common/data/WeaponInfo.xml. The victim's game applies it (victim-owner damage, as for all hits).
// The same goes for our car hitting them at speed and our grenades/rockets going off near them; those knock
// the victim down in their game too. Melee is sync/melee_sync (contact of the striking bones).
// Fire: fire exists only in the game that started it. When we've thrown a molotov / fired the flamethrower
// recently and a partner's copy is burning here while its owner reports not burning, we send them a
// kWeaponFire hit (small damage); their game applies it and sets them alight (START_CHAR_FIRE), after which
// they burn natively and everyone sees the owner's on-fire flag (puppet mirror_fire). Burning cars and other
// fires are the victim's own game's business (the engine burns them there).

#include <cstdint>
#include <filesystem>

#include "game/natives.h"

namespace ivc::team_fire {

// Reads WeaponInfo.xml (game folder: common/data). Once, at startup.
void load(const std::filesystem::path& game_dir);
// Game thread, once per frame before hits_on(): counts the rounds we fired this frame.
void tick();
constexpr std::uint8_t kWeaponFire = 252, kWeaponExplosion = 253, kWeaponVehicle = 254; // Damage.weapon for non-weapon hits

struct Hit {
  std::uint16_t amount = 0;
  std::uint8_t weapon = 0;
  bool knockdown = false; // the victim ragdolls (car, explosion)
};
// What we did to `target` this frame (amount 0: nothing).
Hit hits_on(natives::Ped target);
// True when our fire has set `target`'s copy alight and its owner isn't burning (rate limited): send them a
// kWeaponFire hit of fire_damage(). Applies whether or not the copy is engine-damageable.
bool fire_hit(natives::Ped target, bool owner_burning);
std::uint16_t fire_damage();

} // namespace ivc::team_fire
