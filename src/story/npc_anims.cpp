#include "story/npc_anims.h"

#include <windows.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "core/log.h"
#include "game/native_hooks.h"
#include "net/bytes.h"
#include "story/story_protocol.h"
#include "story/story_sync.h"

namespace ivc::story::npc_anims {
namespace {
namespace n = natives;
using native_hooks::Call;

constexpr std::uint32_t kClearTasks = 0x4AB470F3, kClearTasksImmediately = 0x3C116620, kHandsUp = 0x68232D31,
                        kRequestAnims = 0x65F874DE, kHaveAnimsLoaded = 0x1D3F681D, kRemoveAnims = 0x55E00E7E,
                        kIsCharPlayingAnim = 0x673E4CD2;
constexpr int kMaxArgs = 12;
constexpr DWORD kLoadTimeoutMs = 6000, kCheckEveryMs = 250, kPlayGraceMs = 1500;
constexpr int kMissesToEnd = 3;

enum class Op : std::uint8_t { Start, End, Clear };
enum class Tag : std::uint8_t { Raw, String, Entity };

// One captured native. `strings`: bitmask of argument slots holding C strings. `dict`: the slot naming an
// animation dictionary to load first. `entity`: a slot holding another networked entity's handle. `watch`: the
// host can tell when it stops (IS_CHAR_PLAYING_ANIM; slot 1 is the animation, slot 2 the dictionary).
struct Spec {
  std::uint32_t hash;
  const char* name;
  std::uint32_t strings;
  int dict, entity;
  bool watch;
};
constexpr Spec kSpecs[] = {
    {0x28EE78D8, "TASK_PLAY_ANIM", 0b110, 2, -1, true},
    {0x71F001D2, "TASK_PLAY_ANIM_FACIAL", 0b110, 2, -1, false},
    {0x52202E76, "TASK_PLAY_ANIM_NON_INTERRUPTABLE", 0b110, 2, -1, true},
    {0x273C2D35, "TASK_PLAY_ANIM_SECONDARY", 0b110, 2, -1, true},
    {0x482B2B74, "TASK_PLAY_ANIM_SECONDARY_IN_CAR", 0b110, 2, -1, true},
    {0x56524B94, "TASK_PLAY_ANIM_SECONDARY_NO_INTERRUPT", 0b110, 2, -1, true},
    {0x34574B2A, "TASK_PLAY_ANIM_SECONDARY_UPPER_BODY", 0b110, 2, -1, true},
    {0x02534709, "TASK_PLAY_ANIM_UPPER_BODY", 0b110, 2, -1, true},
    {0x30BA2716, "TASK_PLAY_ANIM_WITH_ADVANCED_FLAGS", 0b110, 2, -1, true},
    {0x75533E74, "TASK_PLAY_ANIM_WITH_FLAGS", 0b110, 2, -1, true},
    {0x1A122D03, "TASK_PLAY_ANIM_WITH_FLAGS_AND_START_PHASE", 0b110, 2, -1, true},
    {0x264C5448, "TASK_SIT_DOWN", 0, -1, -1, false},
    {0x6CC1560F, "TASK_SIT_DOWN_INSTANTLY", 0, -1, -1, false},
    {0x725654F4, "TASK_SIT_DOWN_ON_NEAREST_OBJECT", 0, -1, -1, false},
    {0x515C3218, "TASK_SIT_DOWN_ON_OBJECT", 0, -1, 3, false},
    {0x2CBE4DAF, "TASK_SIT_DOWN_ON_SEAT", 0, -1, -1, false},
    {0x261F18A3, "TASK_START_SCENARIO_IN_PLACE", 0b10, -1, -1, false},
    {0x743F30B3, "TASK_USE_NEAREST_SCENARIO_TO_POS", 0, -1, -1, false},
    {0x47787A40, "TASK_USE_NEAREST_SCENARIO_TO_POS_WARP", 0, -1, -1, false},
    {kHandsUp, "TASK_HANDS_UP", 0, -1, -1, false},
    {0x29103E08, "TASK_COWER", 0, -1, -1, false},
};

const Spec* spec_for(std::uint32_t hash) {
  for (const Spec& s : kSpecs)
    if (s.hash == hash) return &s;
  return nullptr;
}

struct Arg {
  Tag tag = Tag::Raw;
  std::uint32_t value = 0; // Raw: the slot as the script passed it; Entity: networked id
  Key text{};              // String
};

// A call as it travels: slot 0 (the ped) is the message's entity.
struct Start {
  std::uint32_t entity = 0, hash = 0;
  std::vector<Arg> args; // slots 1..
};

std::vector<std::uint8_t> encode_ref(Op op, std::uint32_t entity) {
  net::Writer w;
  w.put(kMsg).put(op).put(entity);
  return std::move(w.data());
}

std::vector<std::uint8_t> encode(const Start& s) {
  net::Writer w;
  w.put(kMsg).put(Op::Start).put(s.entity).put(s.hash).put(static_cast<std::uint8_t>(s.args.size()));
  for (const Arg& a : s.args) {
    w.put(a.tag);
    if (a.tag == Tag::String) w.put(a.text);
    else w.put(a.value);
  }
  return std::move(w.data());
}

bool decode(std::span<const std::uint8_t> d, Op& op, Start& s) {
  net::Reader r(d);
  std::uint8_t type;
  if (!r.get(type) || !r.get(op) || !r.get(s.entity)) return false;
  if (op != Op::Start) return true;
  std::uint8_t count;
  if (!r.get(s.hash) || !r.get(count) || count > kMaxArgs) return false;
  s.args.resize(count);
  for (Arg& a : s.args) {
    if (!r.get(a.tag)) return false;
    if (a.tag == Tag::String) {
      if (!r.get(a.text)) return false;
      a.text.back() = 0;
    } else if (!r.get(a.value)) {
      return false;
    }
  }
  return spec_for(s.hash) != nullptr;
}

// ---------------------------------------------------------------- host capture

bool g_hosting = false;

struct Captured {
  n::Handle ped;
  std::uint32_t hash;
  std::vector<std::uint32_t> slots; // slot 0 is the ped
  std::vector<std::string> strings; // per slot, the C string for string slots
};
struct Cleared { n::Handle ped; };
std::deque<std::variant<Captured, Cleared>> g_pending; // in call order; resolved by host_tick

// What is running on the host's networked peds (also what a late joiner is sent).
struct Playing {
  std::vector<std::uint8_t> msg;
  std::string anim, dict;
  bool watch = false;
  DWORD since = 0;
  int misses = 0;
};
std::map<std::uint32_t, Playing> g_playing;
DWORD g_last_check = 0;

bool capture(const Call& c) { return g_hosting && is_story_script(c.script); }

void on_task(const Call& c, const Spec& spec) {
  if (!capture(c)) return;
  const auto ped = c.arg<n::Handle>(0);
  if (!ped) return; // inside a task sequence
  Captured cap{ped, spec.hash, {}, {}};
  const int count = std::min(c.arg_count, kMaxArgs + 1);
  for (int i = 0; i < count; ++i) {
    cap.slots.push_back(c.arg<std::uint32_t>(i));
    cap.strings.push_back(i > 0 && (spec.strings >> i & 1) ? c.str(i) : std::string());
  }
  g_pending.push_back(std::move(cap));
}

void on_clear(const Call& c) {
  if (!capture(c)) return;
  if (const auto ped = c.arg<n::Handle>(0)) g_pending.push_back(Cleared{ped});
}

// ---------------------------------------------------------------- client playback

struct Entry {
  Start start;
  std::vector<std::uint8_t> msg;
  DWORD since = GetTickCount();
  DWORD hold_until = 0; // 0: until the call ends some other way
  bool playing = false;
};
std::map<std::uint32_t, Entry> g_entries; // by entity
std::set<std::string> g_dicts;            // dictionaries we requested (released in clear())

void release(sync::EntitySync& ents, std::uint32_t entity) {
  if (auto* d = ents.driver_for_id(entity)) d->hold_scripted_anim(false);
}

// Tries to start the call; false while the ped copy, its driver, the dictionary or an entity argument is missing.
bool try_play(Entry& e, sync::EntitySync& ents) {
  const Spec* spec = spec_for(e.start.hash);
  const n::Handle ped = ents.handle_for_id(e.start.entity);
  sync::Puppet* driver = ents.driver_for_id(e.start.entity);
  if (!spec || !ped || !driver || !n::DoesCharExist(ped) || n::IsCharDead(ped)) return false;
  if (spec->dict >= 0 && spec->dict <= static_cast<int>(e.start.args.size())) {
    const std::string dict = to_string(e.start.args[spec->dict - 1].text);
    if (!dict.empty()) {
      if (g_dicts.insert(dict).second || !n::invoke<bool>(kHaveAnimsLoaded, dict.c_str()))
        n::invoke(kRequestAnims, dict.c_str());
      if (!n::invoke<bool>(kHaveAnimsLoaded, dict.c_str())) return false;
    }
  }
  std::uint32_t slots[kMaxArgs + 1] = {ped};
  int i = 1;
  for (const Arg& a : e.start.args) {
    if (a.tag == Tag::String) {
      slots[i++] = reinterpret_cast<std::uint32_t>(a.text.data());
    } else if (a.tag == Tag::Entity) {
      const n::Handle h = ents.handle_for_id(a.value);
      if (!h) return false;
      slots[i++] = h;
    } else {
      slots[i++] = a.value;
    }
  }
  n::invoke_raw(e.start.hash, slots, i);
  driver->hold_scripted_anim(true);
  if (e.start.hash == kHandsUp && !e.start.args.empty()) e.hold_until = GetTickCount() + e.start.args[0].value + 500;
  log::info("NPC anim: entity {} {} ({} args)", e.start.entity, spec->name, e.start.args.size());
  return true;
}

} // namespace

void register_hooks() {
  for (const Spec& spec : kSpecs)
    native_hooks::on(spec.hash, spec.name, [sp = &spec](const Call& c) { on_task(c, *sp); });
  native_hooks::on(kClearTasks, "CLEAR_CHAR_TASKS", on_clear);
  native_hooks::on(kClearTasksImmediately, "CLEAR_CHAR_TASKS_IMMEDIATELY", on_clear);
}

void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents) {
  g_hosting = hosting;
  if (!hosting) {
    g_pending.clear();
    g_playing.clear();
    return;
  }
  while (!g_pending.empty()) {
    auto item = std::move(g_pending.front());
    g_pending.pop_front();
    if (const auto* clr = std::get_if<Cleared>(&item)) {
      const auto id = ents.id_for_handle(clr->ped);
      if (id && g_playing.erase(*id)) net.send_game(encode_ref(Op::Clear, *id), true);
      continue;
    }
    const auto& cap = std::get<Captured>(item);
    const auto id = ents.id_for_handle(cap.ped);
    const Spec* spec = spec_for(cap.hash);
    if (!id || !spec) continue; // not a networked ped (or already gone)
    Start s;
    s.entity = *id, s.hash = cap.hash;
    for (std::size_t i = 1; i < cap.slots.size(); ++i) {
      Arg a;
      if (spec->strings >> i & 1) {
        a.tag = Tag::String, a.text = make_key(cap.strings[i]);
      } else if (static_cast<int>(i) == spec->entity) {
        const auto other = ents.id_for_handle(cap.slots[i]);
        if (!other) break; // an object we don't replicate: nothing to sit on over there
        a.tag = Tag::Entity, a.value = *other;
      } else {
        a.value = cap.slots[i];
      }
      s.args.push_back(a);
    }
    if (s.args.size() + 1 != cap.slots.size()) continue;
    Playing p;
    p.msg = encode(s);
    p.watch = spec->watch && cap.strings.size() > 2;
    if (p.watch) p.anim = cap.strings[1], p.dict = cap.strings[2];
    p.since = GetTickCount();
    net.send_game(p.msg, true);
    g_playing[*id] = std::move(p);
  }
  // A play-anim that stopped playing ends the hold on joiners (the puppet resumes normal movement).
  const DWORD now = GetTickCount();
  if (now - g_last_check < kCheckEveryMs) return;
  g_last_check = now;
  for (auto it = g_playing.begin(); it != g_playing.end();) {
    Playing& p = it->second;
    const n::Handle ped = ents.handle_for_id(it->first);
    if (!ped || !n::DoesCharExist(ped)) {
      it = g_playing.erase(it);
      continue;
    }
    if (p.watch && now - p.since > kPlayGraceMs) {
      p.misses = n::invoke<bool>(kIsCharPlayingAnim, ped, p.dict.c_str(), p.anim.c_str()) ? 0 : p.misses + 1;
      if (p.misses >= kMissesToEnd) {
        net.send_game(encode_ref(Op::End, it->first), true);
        it = g_playing.erase(it);
        continue;
      }
    }
    ++it;
  }
}

void host_on_player_joined(net::Session& net) {
  for (const auto& [id, p] : g_playing) net.send_game(p.msg, true);
}

void client_apply(std::span<const std::uint8_t> msg, sync::EntitySync& ents) {
  Op op;
  Start s;
  if (!decode(msg, op, s)) return;
  if (op == Op::Start) {
    if (const auto it = g_entries.find(s.entity);
        it != g_entries.end() && std::equal(msg.begin(), msg.end(), it->second.msg.begin(), it->second.msg.end()))
      return; // a late-join resend of what is already running
    release(ents, s.entity);
    Entry e;
    e.msg.assign(msg.begin(), msg.end());
    e.start = std::move(s);
    const std::uint32_t id = e.start.entity;
    g_entries[id] = std::move(e);
    return;
  }
  if (op == Op::Clear)
    if (const n::Handle ped = ents.handle_for_id(s.entity); ped && n::DoesCharExist(ped)) n::ClearCharTasks(ped);
  release(ents, s.entity);
  g_entries.erase(s.entity);
}

void client_tick(sync::EntitySync& ents) {
  const DWORD now = GetTickCount();
  for (auto it = g_entries.begin(); it != g_entries.end();) {
    Entry& e = it->second;
    const n::Handle ped = ents.handle_for_id(e.start.entity);
    sync::Puppet* driver = ents.driver_for_id(e.start.entity);
    bool done = false;
    if (!e.playing) {
      if (try_play(e, ents)) e.playing = true;
      else if (now - e.since > kLoadTimeoutMs) done = true;
    } else {
      // The puppet lets go when the host's ped walks off; a dead or vanished copy ends it too.
      done = !driver || !driver->scripted_anim_held() || !ped || n::IsCharDead(ped) ||
             (e.hold_until && now > e.hold_until);
    }
    if (done) {
      if (driver) driver->hold_scripted_anim(false);
      it = g_entries.erase(it);
    } else {
      ++it;
    }
  }
}

void clear() {
  g_pending.clear();
  g_playing.clear();
  g_entries.clear();
  for (const std::string& dict : g_dicts) n::invoke(kRemoveAnims, dict.c_str());
  g_dicts.clear();
}

} // namespace ivc::story::npc_anims
