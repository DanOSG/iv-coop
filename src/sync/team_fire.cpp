#include "sync/team_fire.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <windows.h>

#include "core/log.h"

namespace ivc::team_fire {
namespace {
namespace n = natives;
using n::invoke;

constexpr std::uint32_t kIsTargettingChar = 0x58A6457C, kIsFreeAimingAtChar = 0x30D427B4,
                        kIsCharTouchingVehicle = 0x307A4B8E, kIsExplosionInSphere = 0x47A77D2E,
                        kGetCarSpeed = 0x16DD2D00, kIsCharOnFire = 0x358E21C5;

struct Weapon {
  float damage = 0; // per round, player vs player (base x networkplayermod)
  float range = 0;
};
std::map<std::uint32_t, Weapon> g_weapons; // by weapon type id

// WeaponInfo.xml names -> weapon type ids (eWeaponType, as GIVE_WEAPON_TO_CHAR takes them).
const std::map<std::string, std::uint32_t> kTypeIds = {
    {"UNARMED", 0},  {"BASEBALLBAT", 1}, {"POOLCUE", 2}, {"KNIFE", 3},       {"GRENADE", 4},  {"MOLOTOV", 5},
    {"ROCKET", 6},   {"PISTOL", 7},      {"DEAGLE", 9},  {"SHOTGUN", 10},    {"BARETTA", 11}, {"MICRO_UZI", 12},
    {"MP5", 13},     {"AK47", 14},       {"M4", 15},     {"SNIPERRIFLE", 16}, {"M40A1", 17},  {"RLAUNCHER", 18},
    {"FTHROWER", 19}, {"MINIGUN", 20}};

std::string attr(const std::string& tag, const std::string& name) {
  const std::string key = name + "=\"";
  const auto at = tag.find(key);
  if (at == std::string::npos) return {};
  const auto start = at + key.size();
  return tag.substr(start, tag.find('"', start) - start);
}

float to_float(const std::string& s, float def) {
  try {
    return s.empty() ? def : std::stof(s);
  } catch (...) {
    return def;
  }
}

std::uint32_t g_weapon = 0, g_ammo = 0;
std::uint32_t g_rounds = 0; // fired this frame
n::Vehicle g_car = 0;
float g_car_speed = 0;
DWORD g_last_explosive = 0, g_last_fire_weapon = 0; // explosives; molotovs thrown / flamethrower fired

struct Cooldowns {
  DWORD car = 0, explosion = 0, fire = 0;
};
std::map<n::Ped, Cooldowns> g_cooldowns;

constexpr float kCarHitSpeed = 4.0f; // m/s
constexpr DWORD kExplosiveWindowMs = 8000;
constexpr DWORD kFireWindowMs = 25000; // fire near a partner is ours this long after we threw/sprayed fire
constexpr DWORD kFireReportMs = 2500;  // between fire reports per partner
constexpr std::uint16_t kFireHit = 12; // each report; the victim then burns on their own, taking native damage
// Explosion damage by distance (IS_EXPLOSION_IN_SPHERE, tightest first).
struct Ring {
  float radius;
  std::uint16_t damage;
};
constexpr Ring kExplosionRings[] = {{2.5f, 160}, {4.5f, 90}, {7.0f, 35}};

} // namespace

void load(const std::filesystem::path& game_dir) {
  std::ifstream in(game_dir / "common" / "data" / "WeaponInfo.xml");
  if (!in) {
    log::warn("Team fire: common/data/WeaponInfo.xml not found; shots on player-ped partners won't register");
    return;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string xml = ss.str();
  std::size_t pos = 0;
  while ((pos = xml.find("<weapon type=\"", pos)) != std::string::npos) {
    const std::size_t end = xml.find("</weapon>", pos);
    const std::string block = xml.substr(pos, end - pos);
    pos = end;
    std::string type = attr(block.substr(0, block.find('>')), "type");
    for (char& c : type) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto id = kTypeIds.find(type);
    if (id == kTypeIds.end()) continue;
    const auto data = block.find("<data ");
    const auto damage = block.find("<damage ");
    if (data == std::string::npos || damage == std::string::npos) continue;
    const std::string data_tag = block.substr(data, block.find('>', data) - data);
    const std::string damage_tag = block.substr(damage, block.find('>', damage) - damage);
    Weapon w;
    w.damage = to_float(attr(damage_tag, "base"), 0) * to_float(attr(damage_tag, "networkplayermod"), 1);
    w.range = to_float(attr(data_tag, "weaponrange"), 50);
    g_weapons[id->second] = w;
  }
  log::info("Team fire: {} weapons from WeaponInfo.xml (pistol {:.0f}, shotgun {:.0f}, AK {:.0f} per round)",
            g_weapons.size(), g_weapons[7].damage, g_weapons[10].damage, g_weapons[14].damage);
}

void tick() {
  g_rounds = 0;
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me || !n::DoesCharExist(me)) return;
  std::uint32_t weapon = 0;
  invoke(0x5AB8289F, me, &weapon); // GET_CURRENT_CHAR_WEAPON
  const std::uint32_t ammo = weapon ? n::GetAmmoInCharWeapon(me, weapon) : 0;
  if (weapon && weapon == g_weapon && ammo < g_ammo) g_rounds = std::min<std::uint32_t>(g_ammo - ammo, 10);
  // Explosive weapons fired (rockets) or thrown (grenades, which use up the weapon).
  if (g_rounds && (weapon == 4 || weapon == 18)) g_last_explosive = GetTickCount();
  if (g_weapon == 4 && (weapon != 4 || ammo < g_ammo)) g_last_explosive = GetTickCount();
  // Fire we started: a molotov thrown (it uses up the weapon, like a grenade) or the flamethrower firing.
  if (g_rounds && weapon == 19) g_last_fire_weapon = GetTickCount();
  if (g_weapon == 5 && (weapon != 5 || ammo < g_ammo)) g_last_fire_weapon = GetTickCount();
  g_weapon = weapon, g_ammo = ammo;

  // Our car, if we're driving one.
  g_car = 0, g_car_speed = 0;
  if (n::IsCharInAnyCar(me)) {
    const n::Vehicle car = n::StoreCarCharIsInNoSave(me);
    if (car && n::GetDriverOfCar(car) == me) {
      g_car = car;
      invoke(kGetCarSpeed, car, &g_car_speed);
    }
  }
}

Hit hits_on(n::Ped target) {
  Hit hit;
  if (!target || !n::DoesCharExist(target)) return hit;
  const n::Player player = n::GetPlayerId();
  const n::Ped me = n::GetPlayerChar(player);
  const n::Vector3 a = n::GetCharCoordinates(me), b = n::GetCharCoordinates(target);
  const float dist = std::hypot(a.x - b.x, a.y - b.y, a.z - b.z);
  const DWORD now = GetTickCount();
  Cooldowns& cd = g_cooldowns[target];
  const bool aimed = invoke<bool>(kIsTargettingChar, player, target) || invoke<bool>(kIsFreeAimingAtChar, player, target);

  // Rounds fired at them.
  if (g_rounds && aimed) {
    const auto w = g_weapons.find(g_weapon);
    if (w != g_weapons.end() && w->second.damage > 0 && dist <= w->second.range) {
      hit.amount += static_cast<std::uint16_t>(std::min(w->second.damage * g_rounds, 1000.0f));
      hit.weapon = static_cast<std::uint8_t>(g_weapon);
    }
  }
  // Our car at speed.
  if (g_car && g_car_speed > kCarHitSpeed && now - cd.car > 1000 && invoke<bool>(kIsCharTouchingVehicle, target, g_car)) {
    cd.car = now;
    hit.amount += static_cast<std::uint16_t>(std::min((g_car_speed - kCarHitSpeed + 2.0f) * 8.0f, 160.0f));
    hit.weapon = kWeaponVehicle, hit.knockdown = true;
  }
  // Our explosive (fired or thrown in the last few seconds) going off near them.
  if (now - g_last_explosive < kExplosiveWindowMs && now - cd.explosion > 1500) {
    for (const Ring& ring : kExplosionRings)
      if (invoke<bool>(kIsExplosionInSphere, -1, b.x, b.y, b.z, ring.radius)) {
        cd.explosion = now;
        hit.amount += ring.damage;
        hit.weapon = kWeaponExplosion, hit.knockdown = true;
        break;
      }
  }
  return hit;
}

bool fire_hit(n::Ped target, bool owner_burning) {
  if (!target || !n::DoesCharExist(target) || owner_burning) return false;
  const DWORD now = GetTickCount();
  Cooldowns& cd = g_cooldowns[target];
  // The copy is alight here and the owner isn't: our fire lit it (their game has no fire). Tell them; they
  // catch fire themselves and take the burn damage natively (their on-fire flag then stops these reports).
  if (!g_last_fire_weapon || now - g_last_fire_weapon > kFireWindowMs || now - cd.fire < kFireReportMs) return false;
  if (!invoke<bool>(kIsCharOnFire, target)) return false;
  cd.fire = now;
  return true;
}

std::uint16_t fire_damage() { return kFireHit; }

} // namespace ivc::team_fire
