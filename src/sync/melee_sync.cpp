#include "sync/melee_sync.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "net/bytes.h"
#include "sync/downed.h"

namespace ivc::melee_sync {
namespace {
namespace n = natives;
using n::invoke;

constexpr std::uint32_t kIsCharPlayingAnim = 0x673E4CD2, kRequestAnims = 0x65F874DE, kHaveAnimsLoaded = 0x1D3F681D,
                        kTaskPlayAnim = 0x28EE78D8, kTaskPlayAnimSecondary = 0x273C2D35,
                        kIsCharInMeleeCombat = 0x68855BE7, kGetPedBonePosition = 0x43475BB3,
                        kPlaySoundFromPed = 0x56F37A81;
constexpr DWORD kMirrorMs = 1500;     // longest we let a mirrored clip hold a copy
constexpr float kBodyRadius = 0.18f;  // around each of the target's head/neck/chest bones
constexpr float kDamageScale = 2.0f;  // the table's damage is tuned for peds; players have 100 to lose
const std::string kLocomotion = "move_melee";

// Bone tags (IV ped skeleton; head 1205 / neck 1204 / left hand 1232 as scripts use them).
const std::map<std::string, std::uint32_t> kBones = {
    {"HEAD", 1205},       {"NECK", 1204},       {"L_UPPERARM", 1220}, {"L_FOREARM", 1230}, {"L_HAND", 1232},
    {"R_UPPERARM", 1250}, {"R_FOREARM", 1260}, {"R_HAND", 1262},     {"L_THIGH", 1300},   {"L_CALF", 1310},
    {"L_FOOT", 1320},     {"R_THIGH", 1340},    {"R_CALF", 1350},     {"R_FOOT", 1360}};
constexpr std::uint32_t kTargetBones[] = {1205, 1204, 1203, 1201}; // head, neck, chest, belly

struct Clip {
  std::string dict, name;
};
struct Strike {
  std::uint32_t bones[2] = {0, 0};
  float radii[2] = {0, 0};
  std::uint16_t damage = 0;
  std::string recoil; // clip name in the same dictionary ("" none)
  std::string sound;  // MELEE_COMBAT_* ("" none)
};

std::vector<Clip> g_clips;               // every melee clip but the locomotion set
std::map<std::string, Strike> g_strikes; // by lower-case "dict/clip"
int g_current = -1;                      // the clip our player is playing
DWORD g_melee_until = 0;

struct Mirror {
  Clip clip;
  DWORD started = 0;
  std::uint8_t attacker = 0xFF;
  bool connected = false; // this swing already touched us
};
std::map<n::Ped, Mirror> g_mirrors; // copies playing a partner's clip

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string key(const std::string& dict, const std::string& clip) { return lower(dict) + "/" + lower(clip); }

bool is_playing(n::Ped ped, const Clip& c) { return invoke<bool>(kIsCharPlayingAnim, ped, c.dict.c_str(), c.name.c_str()); }

int find_clip(const std::string& dict, const std::string& name) {
  for (std::size_t i = 0; i < g_clips.size(); ++i)
    if (_stricmp(g_clips[i].dict.c_str(), dict.c_str()) == 0 && _stricmp(g_clips[i].name.c_str(), name.c_str()) == 0)
      return static_cast<int>(i);
  return -1;
}

// A copy plays a partner's clip as its main task (fast blend); `overlay` plays it on top of what the ped
// is doing (our own reactions: we keep control). False while the dictionary is still loading.
bool play(n::Ped ped, const Clip& c, bool overlay = false) {
  invoke(kRequestAnims, c.dict.c_str());
  if (!invoke<bool>(kHaveAnimsLoaded, c.dict.c_str())) return false;
  invoke(overlay ? kTaskPlayAnimSecondary : kTaskPlayAnim, ped, c.name.c_str(), c.dict.c_str(), 16.0f, 0, 0, 0, 0, -1);
  return true;
}

void sound(n::Ped ped, const std::string& name) {
  if (!name.empty() && ped && n::DoesCharExist(ped)) invoke(kPlaySoundFromPed, -1, name.c_str(), ped);
}

n::Vector3 bone(n::Ped ped, std::uint32_t tag) {
  n::Vector3 v{};
  invoke(kGetPedBonePosition, ped, tag, 0.0f, 0.0f, 0.0f, &v);
  return v;
}

float dist(const n::Vector3& a, const n::Vector3& b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); }

// The side a blow from `attacker` comes from, relative to `ped`'s facing: -180..180, + = from the left.
float side_of(n::Ped ped, n::Ped attacker) {
  if (!attacker || !n::DoesCharExist(attacker)) return 0;
  const n::Vector3 a = n::GetCharCoordinates(ped), b = n::GetCharCoordinates(attacker);
  const float bearing = std::atan2(-(b.x - a.x), b.y - a.y) * 57.2958f; // IV: degrees, 0 = +y, CCW
  return std::fmod(bearing - n::GetCharHeading(ped) + 540.0f, 360.0f) - 180.0f;
}

std::vector<std::uint8_t> encode_clip(std::uint8_t type, std::uint8_t target, const Clip& c) {
  net::Writer w;
  w.put(type).put(std::uint8_t{0}).put(target).put(static_cast<std::uint8_t>(c.dict.size()));
  for (char ch : c.dict) w.put(ch);
  w.put(static_cast<std::uint8_t>(c.name.size()));
  for (char ch : c.name) w.put(ch);
  return std::move(w.data());
}

bool decode_clip(std::span<const std::uint8_t> d, std::uint8_t& target, Clip& c) {
  net::Reader r(d);
  std::uint8_t type, sender, len;
  if (!r.get(type) || !r.get(sender) || !r.get(target) || !r.get(len)) return false;
  for (int i = 0; i < len; ++i) {
    char ch;
    if (!r.get(ch)) return false;
    c.dict += ch;
  }
  if (!r.get(len)) return false;
  for (int i = 0; i < len; ++i) {
    char ch;
    if (!r.get(ch)) return false;
    c.name += ch;
  }
  return true;
}

std::vector<std::string> split_csv(const std::string& line) {
  std::vector<std::string> out;
  std::string cell;
  std::istringstream s(line);
  while (std::getline(s, cell, ',')) out.push_back(cell);
  return out;
}

void load_clips(const std::filesystem::path& file, std::map<std::string, std::string>& group_dicts) {
  std::ifstream in(file);
  std::string line, dict;
  bool header = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("MELEE_ANIM_GROUP:", 0) == 0) {
      header = true;
      continue;
    }
    if (line.rfind("END_ANIM_GROUP", 0) == 0) {
      dict.clear();
      continue;
    }
    if (line.empty() || line[0] == '#') continue;
    std::istringstream words(line);
    if (header) { // "<group> <dictionary> ..."
      std::string group;
      words >> group >> dict;
      group_dicts[lower(group)] = dict;
      header = false;
      continue;
    }
    std::string clip;
    words >> clip;
    if (!dict.empty() && !clip.empty() && _stricmp(dict.c_str(), kLocomotion.c_str()) != 0) g_clips.push_back({dict, clip});
  }
}

void load_strikes(const std::filesystem::path& file, const std::map<std::string, std::string>& group_dicts) {
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    const auto t = split_csv(line);
    if (t.size() < 10 || t[0].size() < 4 || t[0].compare(t[0].size() - 4, 4, "_Res") != 0) continue;
    const auto group = group_dicts.find(lower(t[2]));
    if (group == group_dicts.end()) continue;
    std::size_t b = 0;
    for (std::size_t i = 4; i + 4 < t.size() && !b; ++i)
      if (kBones.contains(t[i])) b = i;
    if (!b) continue; // not a strike (reactions, idles)
    Strike s;
    for (int k = 0; k < 2; ++k) {
      const auto it = kBones.find(t[b + 2 * k]);
      s.bones[k] = it != kBones.end() ? it->second : 0;
      try {
        s.radii[k] = std::stof(t[b + 2 * k + 1]);
      } catch (...) {
      }
    }
    try {
      s.damage = static_cast<std::uint16_t>(std::stof(t[b + 4]) * kDamageScale);
    } catch (...) {
    }
    if (b + 7 < t.size() && t[b + 7] != "None") {
      std::string recoil = t[b + 7];
      if (recoil.size() > 4 && recoil.compare(recoil.size() - 4, 4, "_Act") == 0) recoil.resize(recoil.size() - 4);
      if (find_clip(group->second, recoil) >= 0) s.recoil = recoil;
    }
    for (auto it = t.rbegin(); it != t.rend(); ++it)
      if (it->rfind("MELEE_COMBAT_", 0) == 0) {
        s.sound = *it;
        break;
      }
    g_strikes.try_emplace(key(group->second, t[3]), s); // the first row for a clip wins
  }
}

// A partner's swing (their copy playing an attack clip here) touched us: we decide - a block (no damage,
// our block reaction; they're told, to play their recoil) or a hit (damage, our hit reaction; they're told,
// for the impact sound).
void resolve_hit(std::uint8_t attacker, n::Ped from_ped, const Clip& c, net::Session& net) {
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  const auto strike = g_strikes.find(key(c.dict, c.name));
  const std::uint16_t damage = strike != g_strikes.end() ? strike->second.damage : 10;
  const float side = side_of(me, from_ped);
  const bool behind = std::fabs(side) > 120.0f;
  const bool blocking = is_playing(me, {"MELEE_UNARMED_BASE", "Block"}) ||
                        is_playing(me, {"MELEE_HITS_COMMON", "DAM_Block_Front"}) ||
                        is_playing(me, {"MELEE_HITS_COMMON", "DAM_Block_Left"}) ||
                        is_playing(me, {"MELEE_HITS_COMMON", "DAM_Block_Right"});
  if (blocking && !behind) {
    play(me, {"MELEE_HITS_COMMON", side > 35 ? "DAM_Block_Left" : side < -35 ? "DAM_Block_Right" : "DAM_Block_Front"},
         true);
    net.send_game(encode_clip(kMsgBlocked, attacker, c), true);
    log::info("Melee: blocked #{}'s {}", attacker, c.name);
    return;
  }
  // The hit reaction: the attack's own one if the base set has it (Hit_Jab, Hit_Cross_R...), else by side.
  std::string reaction = "Hit_" + c.name;
  if (find_clip("MELEE_UNARMED_BASE", reaction) < 0 || behind)
    reaction = behind ? "Hit_Back" : side > 35 ? "Hit_Left" : side < -35 ? "Hit_Right" : "Hit_Jab";
  play(me, {"MELEE_UNARMED_BASE", reaction}, true);
  if (strike != g_strikes.end()) sound(me, strike->second.sound);
  net.send_game(encode_clip(kMsgHit, attacker, c), true);
  const std::uint32_t h = n::GetCharHealth(me);
  n::SetCharHealth(me, downed::floor_health(h > damage ? h - damage : 0));
  log::info("Melee: took {} from #{}'s {}, health {} -> {}", damage, attacker, c.name, h, h > damage ? h - damage : 0);
}

// Partners' swings as we see them: each copy playing an attack clip has its striking bones tested against
// our head/neck/chest every frame; the first touch is the hit (once per swing). What lands is what our
// screen shows - stepping out of reach or dodging works.
void check_incoming(net::Session& net) {
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me || n::IsCharDead(me)) return;
  for (auto& [ped, mirror] : g_mirrors) {
    if (mirror.connected || !n::DoesCharExist(ped)) continue;
    const auto strike = g_strikes.find(key(mirror.clip.dict, mirror.clip.name));
    if (strike == g_strikes.end()) continue;
    for (int k = 0; k < 2 && !mirror.connected; ++k) {
      if (!strike->second.bones[k]) continue;
      const n::Vector3 fist = bone(ped, strike->second.bones[k]);
      for (std::uint32_t tag : kTargetBones)
        if (dist(fist, bone(me, tag)) < strike->second.radii[k] + kBodyRadius) {
          mirror.connected = true;
          resolve_hit(mirror.attacker, ped, mirror.clip, net);
          break;
        }
    }
  }
}

void log_bones_once(n::Ped me) {
  static bool done = false;
  if (done) return;
  done = true;
  const n::Vector3 root = n::GetCharCoordinates(me);
  std::string out;
  for (const auto& [name, tag] : kBones) out += std::format(" {}={:.2f}", name, dist(bone(me, tag), root));
  log::info("Melee: bone distances from the ped origin (sanity check of the tags):{}", out);
}

} // namespace

void load(const std::filesystem::path& game_dir) {
  const auto data = game_dir / "common" / "data";
  std::map<std::string, std::string> group_dicts;
  load_clips(data / "MeleeAnims.dat", group_dicts);
  load_strikes(data / "action_table.csv", group_dicts);
  log::info("Melee: {} clips (MeleeAnims.dat), {} strikes with contact bones (action_table.csv)", g_clips.size(),
            g_strikes.size());
}

void tick(bool in_session, bool can_hit, net::Session& net) {
  if (!in_session || g_clips.empty()) return;
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me || !n::DoesCharExist(me)) return;
  const DWORD now = GetTickCount();
  if (invoke<bool>(kIsCharInMeleeCombat, me)) g_melee_until = now + 1500, log_bones_once(me);
  int found = -1;
  if (now < g_melee_until || g_current >= 0) {
    if (g_current >= 0 && is_playing(me, g_clips[g_current])) found = g_current;
    else
      for (std::size_t i = 0; i < g_clips.size() && found < 0; ++i)
        if (is_playing(me, g_clips[i])) found = static_cast<int>(i);
  }
  if (found != g_current && found >= 0) net.send_game(encode_clip(kMsgClip, 0xFF, g_clips[found]), true);
  g_current = found;
  if (can_hit) check_incoming(net);

  for (auto it = g_mirrors.begin(); it != g_mirrors.end();) {
    const bool over = now - it->second.started > kMirrorMs ||
                      (now - it->second.started > 150 && n::DoesCharExist(it->first) && !is_playing(it->first, it->second.clip));
    it = over ? g_mirrors.erase(it) : std::next(it);
  }
}

void on_message(const net::GameMessage& m, std::uint8_t my_id, n::Ped from_ped) {
  std::uint8_t target = 0xFF;
  Clip c;
  if (!decode_clip(m.data, target, c)) return;
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  switch (m.data[0]) {
    case kMsgClip:
      if (find_clip(c.dict, c.name) >= 0 && from_ped && n::DoesCharExist(from_ped) && play(from_ped, c))
        g_mirrors[from_ped] = {c, GetTickCount(), m.from, false};
      break;

    case kMsgHit: { // our swing landed on them (their screen decided): the impact, on their copy here
      if (target != my_id) break;
      const auto strike = g_strikes.find(key(c.dict, c.name));
      if (strike != g_strikes.end()) sound(from_ped, strike->second.sound);
      log::info("Melee: our {} landed on #{}", c.name, m.from);
      break;
    }

    case kMsgBlocked: { // our swing was blocked: the fist bounces off
      if (target != my_id || !me) break;
      const auto strike = g_strikes.find(key(c.dict, c.name));
      if (strike != g_strikes.end() && !strike->second.recoil.empty()) play(me, {c.dict, strike->second.recoil});
      log::info("Melee: our {} was blocked by #{}", c.name, m.from);
      break;
    }
  }
}

bool playing(n::Ped ped) { return g_mirrors.contains(ped); }

} // namespace ivc::melee_sync
