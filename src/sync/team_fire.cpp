#include "sync/team_fire.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "core/log.h"

namespace ivc::team_fire {
namespace {
namespace n = natives;
using n::invoke;

constexpr std::uint32_t kIsTargettingChar = 0x58A6457C, kIsFreeAimingAtChar = 0x30D427B4;

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
  g_weapon = weapon, g_ammo = ammo;
}

std::uint16_t hits_on(n::Ped target) {
  if (!g_rounds || !target || !n::DoesCharExist(target)) return 0;
  const auto w = g_weapons.find(g_weapon);
  if (w == g_weapons.end() || w->second.damage <= 0) return 0;
  const n::Player player = n::GetPlayerId();
  if (!invoke<bool>(kIsTargettingChar, player, target) && !invoke<bool>(kIsFreeAimingAtChar, player, target)) return 0;
  const n::Vector3 a = n::GetCharCoordinates(n::GetPlayerChar(player)), b = n::GetCharCoordinates(target);
  if (std::hypot(a.x - b.x, a.y - b.y, a.z - b.z) > w->second.range) return 0;
  return static_cast<std::uint16_t>(std::min(w->second.damage * g_rounds, 1000.0f));
}

} // namespace ivc::team_fire
