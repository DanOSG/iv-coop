#include "sync/melee_sync.h"

#include <windows.h>

#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "core/log.h"
#include "net/bytes.h"

namespace ivc::melee_sync {
namespace {
namespace n = natives;
using n::invoke;

constexpr std::uint32_t kIsCharPlayingAnim = 0x673E4CD2, kRequestAnims = 0x65F874DE, kHaveAnimsLoaded = 0x1D3F681D,
                        kTaskPlayAnim = 0x28EE78D8, kIsCharInMeleeCombat = 0x68855BE7;
constexpr DWORD kMirrorMs = 1500; // longest we let a mirrored clip hold the copy

struct Clip {
  std::string dict, name;
};
std::vector<Clip> g_clips;
int g_current = -1;         // the clip our player is playing (index), -1 none
DWORD g_melee_until = 0;    // keep scanning briefly after melee combat ends (reactions)

struct Mirror {
  Clip clip;
  DWORD started = 0;
};
std::map<n::Ped, Mirror> g_mirrors; // copies playing a partner's clip

bool is_playing(n::Ped ped, const Clip& c) {
  return invoke<bool>(kIsCharPlayingAnim, ped, c.dict.c_str(), c.name.c_str());
}

int find_clip(const std::string& dict, const std::string& name) {
  for (std::size_t i = 0; i < g_clips.size(); ++i)
    if (_stricmp(g_clips[i].dict.c_str(), dict.c_str()) == 0 && _stricmp(g_clips[i].name.c_str(), name.c_str()) == 0)
      return static_cast<int>(i);
  return -1;
}

std::vector<std::uint8_t> encode(const Clip& c) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(static_cast<std::uint8_t>(c.dict.size()));
  for (char ch : c.dict) w.put(ch);
  w.put(static_cast<std::uint8_t>(c.name.size()));
  for (char ch : c.name) w.put(ch);
  return std::move(w.data());
}

// Plays a clip on `ped` once the dictionary is loaded (requested on the way); false if not loaded yet.
bool play(n::Ped ped, const Clip& c) {
  invoke(kRequestAnims, c.dict.c_str());
  if (!invoke<bool>(kHaveAnimsLoaded, c.dict.c_str())) return false;
  invoke(kTaskPlayAnim, ped, c.name.c_str(), c.dict.c_str(), 8.0f, 0, 0, 0, 0, -1);
  return true;
}

} // namespace

void load(const std::filesystem::path& game_dir) {
  std::ifstream in(game_dir / "common" / "data" / "MeleeAnims.dat");
  if (!in) {
    log::warn("Melee: common/data/MeleeAnims.dat not found; melee moves won't be mirrored");
    return;
  }
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
      header = false;
      continue;
    }
    std::string clip;
    words >> clip;
    if (!dict.empty() && !clip.empty()) g_clips.push_back({dict, clip});
  }
  log::info("Melee: {} clips from MeleeAnims.dat", g_clips.size());
}

void tick(bool in_session, net::Session& net) {
  if (!in_session || g_clips.empty()) return;
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me || !n::DoesCharExist(me)) return;
  const DWORD now = GetTickCount();
  if (invoke<bool>(kIsCharInMeleeCombat, me)) g_melee_until = now + 1500;
  int found = -1;
  if (now < g_melee_until || g_current >= 0) {
    if (g_current >= 0 && is_playing(me, g_clips[g_current])) found = g_current; // still the same one
    else
      for (std::size_t i = 0; i < g_clips.size() && found < 0; ++i)
        if (is_playing(me, g_clips[i])) found = static_cast<int>(i);
  }
  if (found != g_current && found >= 0) net.send_game(encode(g_clips[found]), true);
  g_current = found;

  // Mirrored clips that ended free their copy.
  for (auto it = g_mirrors.begin(); it != g_mirrors.end();) {
    const bool over = now - it->second.started > kMirrorMs ||
                      (now - it->second.started > 150 && n::DoesCharExist(it->first) && !is_playing(it->first, it->second.clip));
    it = over ? g_mirrors.erase(it) : std::next(it);
  }
}

void on_message(const net::GameMessage& m, n::Ped ped, std::uint32_t now_ms) {
  net::Reader r(m.data);
  std::uint8_t type, sender, len;
  Clip c;
  if (!r.get(type) || !r.get(sender) || !r.get(len)) return;
  for (int i = 0; i < len; ++i) {
    char ch;
    if (!r.get(ch)) return;
    c.dict += ch;
  }
  if (!r.get(len)) return;
  for (int i = 0; i < len; ++i) {
    char ch;
    if (!r.get(ch)) return;
    c.name += ch;
  }
  if (find_clip(c.dict, c.name) < 0 || !ped || !n::DoesCharExist(ped)) return; // only clips the game lists
  if (play(ped, c)) g_mirrors[ped] = {c, now_ms};
}

bool playing(n::Ped ped) { return g_mirrors.contains(ped); }

bool react_to_hit(n::Ped attacker) {
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me || !n::DoesCharExist(me)) return false;
  static const Clip kBlock{"MELEE_UNARMED_BASE", "Block"};
  const bool blocking = is_playing(me, kBlock) || is_playing(me, {"MELEE_HITS_COMMON", "DAM_Block_Front"}) ||
                        is_playing(me, {"MELEE_HITS_COMMON", "DAM_Block_Left"}) ||
                        is_playing(me, {"MELEE_HITS_COMMON", "DAM_Block_Right"});
  // Which side the blow came from: the attacker's bearing against our heading (IV: degrees, 0 = +y, CCW).
  float side = 0;
  if (attacker && n::DoesCharExist(attacker)) {
    const n::Vector3 a = n::GetCharCoordinates(me), b = n::GetCharCoordinates(attacker);
    const float bearing = std::atan2(-(b.x - a.x), b.y - a.y) * 57.2958f;
    side = std::fmod(bearing - n::GetCharHeading(me) + 540.0f, 360.0f) - 180.0f; // -180..180, + = left
  }
  const bool behind = std::fabs(side) > 120.0f;
  if (blocking && !behind) {
    play(me, {"MELEE_HITS_COMMON", side > 35 ? "DAM_Block_Left" : side < -35 ? "DAM_Block_Right" : "DAM_Block_Front"});
    return true;
  }
  play(me, {"MELEE_UNARMED_BASE", behind ? "Hit_Back" : side > 35 ? "Hit_Left" : side < -35 ? "Hit_Right" : "Hit_Jab"});
  return false;
}

} // namespace ivc::melee_sync
