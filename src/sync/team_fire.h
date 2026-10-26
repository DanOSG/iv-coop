#pragma once
// Our shots on partners who are player peds.
//
// Partners are extra player peds (game/player_slots), and the single-player engine never applies
// player-on-player damage: WeaponInfo.xml's per-weapon "networkplayermod" shows that path exists only in
// network games. So we detect our own hits: when a round leaves our gun (ammo drops) while we're locked
// on to or free-aiming at a partner within the weapon's range, that partner is hit for the weapon's
// player-vs-player damage, base x networkplayermod - Rockstar's own multiplayer numbers, read from the
// game's common/data/WeaponInfo.xml. The victim's game applies it (victim-owner damage, as for all hits).
// Not covered yet: melee, explosives, fire and vehicles against player-ped partners.

#include <cstdint>
#include <filesystem>

#include "game/natives.h"

namespace ivc::team_fire {

// Reads WeaponInfo.xml (game folder: common/data). Once, at startup.
void load(const std::filesystem::path& game_dir);
// Game thread, once per frame before hits_on(): counts the rounds we fired this frame.
void tick();
// Damage our shots this frame did to `target` (0 if none).
std::uint16_t hits_on(natives::Ped target);

} // namespace ivc::team_fire
