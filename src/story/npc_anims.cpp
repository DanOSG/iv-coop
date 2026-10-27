#include "story/npc_anims.h"

#include <windows.h>

#include <algorithm>
#include <array>
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
                        kIsCharPlayingAnim = 0x673E4CD2, kOpenSequence = 0x14A67125, kCloseSequence = 0x16C1B04,
                        kClearSequence = 0x7ED774FE, kPerformSequence = 0x36A33C21,
                        kPerformSequenceFromProgress = 0x62701AF8, kGetSequenceProgress = 0x1FBD3ACA;
constexpr int kMaxArgs = 12;
constexpr int kMaxSteps = 16;
constexpr std::size_t kMaxSequenceBytes = 1100;
constexpr DWORD kSequenceMaxHoldMs = 180000; // safety net if the End never arrives
constexpr DWORD kLoadTimeoutMs = 6000, kCheckEveryMs = 250, kPlayGraceMs = 1500;
constexpr int kMissesToEnd = 3;

enum class Op : std::uint8_t { Start, End, Clear, Sequence };
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

// Movement tasks: only replayed as steps of a task sequence (a lone call is the movement stream's business).
constexpr Spec kMoveSpecs[] = {
    {0x6D6A1261, "TASK_ACHIEVE_HEADING", 0, -1, -1, false},
    {0x04962F82, "TASK_CHAR_SLIDE_TO_COORD", 0, -1, -1, false},
    {0x1B31390E, "TASK_FOLLOW_NAV_MESH_TO_COORD", 0, -1, -1, false},
    {0x19591255, "TASK_GO_STRAIGHT_TO_COORD", 0, -1, -1, false},
    {0x524C4CB5, "TASK_STAND_STILL", 0, -1, -1, false},
    {0x51517B11, "TASK_TURN_CHAR_TO_FACE_COORD", 0, -1, -1, false},
    {0x5E702E2C, "TASK_PAUSE", 0, -1, -1, false},
};

const Spec* spec_for(std::uint32_t hash) {
  for (const Spec& s : kSpecs)
    if (s.hash == hash) return &s;
  return nullptr;
}

// Anything allowed as a sequence step.
const Spec* step_spec_for(std::uint32_t hash) {
  if (const Spec* s = spec_for(hash)) return s;
  for (const Spec& s : kMoveSpecs)
    if (s.hash == hash) return &s;
  return nullptr;
}

struct Arg {
  Tag tag = Tag::Raw;
  std::uint32_t value = 0; // Raw: the slot as the script passed it; Entity: networked id
  Key text{};              // String
};

// One native of a task sequence: slot 0 (the ped) is left out.
struct Step {
  std::uint32_t hash = 0;
  std::vector<Arg> args; // slots 1..
};

// A call as it travels: slot 0 (the ped) is the message's entity. A sequence (Op::Sequence) carries `steps`
// (performed from step `first`) instead of one hash.
struct Start {
  std::uint32_t entity = 0, hash = 0;
  std::vector<Arg> args; // slots 1..
  bool seq = false;
  std::uint8_t first = 0;
  std::vector<Step> steps;
};

void put_args(net::Writer& w, const std::vector<Arg>& args) {
  w.put(static_cast<std::uint8_t>(args.size()));
  for (const Arg& a : args) {
    w.put(a.tag);
    if (a.tag == Tag::String) w.put(a.text);
    else w.put(a.value);
  }
}

bool get_args(net::Reader& r, std::vector<Arg>& args) {
  std::uint8_t count;
  if (!r.get(count) || count > kMaxArgs) return false;
  args.resize(count);
  for (Arg& a : args) {
    if (!r.get(a.tag)) return false;
    if (a.tag == Tag::String) {
      if (!r.get(a.text)) return false;
      a.text.back() = 0;
    } else if (!r.get(a.value)) {
      return false;
    }
  }
  return true;
}

std::vector<std::uint8_t> encode_ref(Op op, std::uint32_t entity) {
  net::Writer w;
  w.put(kMsg).put(op).put(entity);
  return std::move(w.data());
}

std::vector<std::uint8_t> encode(const Start& s) {
  net::Writer w;
  w.put(kMsg).put(Op::Start).put(s.entity).put(s.hash);
  put_args(w, s.args);
  return std::move(w.data());
}

std::vector<std::uint8_t> encode_sequence(std::uint32_t entity, std::uint8_t first, const std::vector<Step>& steps) {
  net::Writer w;
  w.put(kMsg).put(Op::Sequence).put(entity).put(first).put(static_cast<std::uint8_t>(steps.size()));
  for (const Step& st : steps) {
    w.put(st.hash);
    put_args(w, st.args);
  }
  return std::move(w.data());
}

bool decode(std::span<const std::uint8_t> d, Op& op, Start& s) {
  net::Reader r(d);
  std::uint8_t type;
  if (!r.get(type) || !r.get(op) || !r.get(s.entity)) return false;
  if (op == Op::Sequence) {
    std::uint8_t count;
    if (!r.get(s.first) || !r.get(count) || count > kMaxSteps) return false;
    s.seq = true;
    s.steps.resize(count);
    for (Step& st : s.steps)
      if (!r.get(st.hash) || !get_args(r, st.args) || !step_spec_for(st.hash)) return false;
    return true;
  }
  if (op != Op::Start) return true;
  if (!r.get(s.hash) || !get_args(r, s.args)) return false;
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
struct PerformSeq {
  n::Handle ped;
  int first;
  std::vector<Captured> steps; // ped = 0
};
std::deque<std::variant<Captured, Cleared, PerformSeq>> g_pending; // in call order; resolved by host_tick

// What is running on the host's networked peds (also what a late joiner is sent).
struct Playing {
  std::vector<std::uint8_t> msg;
  std::string anim, dict;
  bool watch = false, seq = false;
  DWORD since = 0;
  int misses = 0;
};
std::map<std::uint32_t, Playing> g_playing;
DWORD g_last_check = 0;

// Task sequences being recorded: the one a script thread has open, and every recipe since it was opened
// (a script clears its handle right after performing it, so the recipe is copied at perform time).
std::map<std::string, int> g_open;
std::map<int, std::vector<Captured>> g_recipes;

bool capture(const Call& c) { return g_hosting && is_story_script(c.script); }

void on_task(const Call& c, const Spec& spec) {
  if (!capture(c)) return;
  const auto ped = c.arg<n::Handle>(0);
  if (ped && !spec_for(spec.hash)) return; // movement tasks only matter inside sequences
  Captured cap{ped, spec.hash, {}, {}};
  const int count = std::min(c.arg_count, kMaxArgs + 1);
  for (int i = 0; i < count; ++i) {
    cap.slots.push_back(c.arg<std::uint32_t>(i));
    cap.strings.push_back(i > 0 && (spec.strings >> i & 1) ? c.str(i) : std::string());
  }
  if (!ped) { // inside a task sequence: part of the recipe being recorded
    if (const auto open = g_open.find(c.script); open != g_open.end()) {
      auto& recipe = g_recipes[open->second];
      if (recipe.size() < kMaxSteps) recipe.push_back(std::move(cap));
    }
    return;
  }
  g_pending.push_back(std::move(cap));
}

void on_open_sequence(const Call& c) {
  if (!capture(c)) return;
  const int seq = c.out<int>(0);
  g_open[c.script] = seq;
  g_recipes[seq].clear();
}

void on_close_sequence(const Call& c) {
  if (capture(c)) g_open.erase(c.script);
}

void on_clear_sequence(const Call& c) {
  if (capture(c)) g_recipes.erase(c.arg<int>(0));
}

void on_perform_sequence(const Call& c, bool from_progress) {
  if (!capture(c)) return;
  const auto ped = c.arg<n::Handle>(0);
  const auto it = g_recipes.find(c.arg<int>(1));
  if (!ped || it == g_recipes.end() || it->second.empty()) return;
  g_pending.push_back(PerformSeq{ped, from_progress ? std::max(0, c.arg<int>(2)) : 0, it->second});
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

// Loads the animation dictionary a call names; false while it is still loading.
bool dict_ready(const Spec* spec, const std::vector<Arg>& args) {
  if (spec->dict < 0 || spec->dict > static_cast<int>(args.size())) return true;
  const std::string dict = to_string(args[spec->dict - 1].text);
  if (dict.empty()) return true;
  if (g_dicts.insert(dict).second || !n::invoke<bool>(kHaveAnimsLoaded, dict.c_str()))
    n::invoke(kRequestAnims, dict.c_str());
  return n::invoke<bool>(kHaveAnimsLoaded, dict.c_str());
}

// Fills slots 1.. from the call's arguments (slot 0 is the caller's); false while an entity argument is missing.
bool fill_slots(const std::vector<Arg>& args, sync::EntitySync& ents, std::uint32_t* slots, int& i) {
  for (const Arg& a : args) {
    if (a.tag == Tag::String) {
      slots[i++] = reinterpret_cast<std::uint32_t>(a.text.data());
    } else if (a.tag == Tag::Entity) {
      const n::Handle h = ents.handle_for_id(a.value);
      if (!h) return false;
      slots[i++] = static_cast<std::uint32_t>(h);
    } else {
      slots[i++] = a.value;
    }
  }
  return true;
}

// Tries to start the call; false while the ped copy, its driver, the dictionary or an entity argument is missing.
bool try_play(Entry& e, sync::EntitySync& ents) {
  const n::Handle ped = ents.handle_for_id(e.start.entity);
  sync::Puppet* driver = ents.driver_for_id(e.start.entity);
  if (!ped || !driver || !n::DoesCharExist(ped) || n::IsCharDead(ped)) return false;

  if (e.start.seq) {
    bool ready = true;
    for (const Step& st : e.start.steps) ready = dict_ready(step_spec_for(st.hash), st.args) && ready;
    if (!ready) return false;
    // Resolve everything first so a missing entity doesn't leave a half-built sequence open.
    std::vector<std::array<std::uint32_t, kMaxArgs + 1>> all(e.start.steps.size());
    std::vector<int> counts(all.size());
    for (std::size_t k = 0; k < all.size(); ++k) {
      all[k][0] = 0;
      counts[k] = 1;
      if (!fill_slots(e.start.steps[k].args, ents, all[k].data(), counts[k])) return false;
    }
    int seq = 0;
    n::invoke(kOpenSequence, &seq);
    for (std::size_t k = 0; k < all.size(); ++k) n::invoke_raw(e.start.steps[k].hash, all[k].data(), counts[k]);
    n::invoke(kCloseSequence, seq);
    if (e.start.first > 0) n::invoke(kPerformSequenceFromProgress, ped, seq, static_cast<int>(e.start.first), 0);
    else n::invoke(kPerformSequence, ped, seq);
    n::invoke(kClearSequence, seq);
    driver->hold_scripted_anim(true, true);
    e.hold_until = GetTickCount() + kSequenceMaxHoldMs;
    log::info("NPC anim: entity {} sequence of {} steps", e.start.entity, e.start.steps.size());
    return true;
  }

  const Spec* spec = spec_for(e.start.hash);
  if (!spec || !dict_ready(spec, e.start.args)) return false;
  std::uint32_t slots[kMaxArgs + 1] = {static_cast<std::uint32_t>(ped)};
  int i = 1;
  if (!fill_slots(e.start.args, ents, slots, i)) return false;
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
  for (const Spec& spec : kMoveSpecs)
    native_hooks::on(spec.hash, spec.name, [sp = &spec](const Call& c) { on_task(c, *sp); });
  native_hooks::on(kOpenSequence, "OPEN_SEQUENCE_TASK", on_open_sequence);
  native_hooks::on(kCloseSequence, "CLOSE_SEQUENCE_TASK", on_close_sequence);
  native_hooks::on(kClearSequence, "CLEAR_SEQUENCE_TASK", on_clear_sequence);
  native_hooks::on(kPerformSequence, "TASK_PERFORM_SEQUENCE", [](const Call& c) { on_perform_sequence(c, false); });
  native_hooks::on(kPerformSequenceFromProgress, "TASK_PERFORM_SEQUENCE_FROM_PROGRESS",
                   [](const Call& c) { on_perform_sequence(c, true); });
  native_hooks::on(kClearTasks, "CLEAR_CHAR_TASKS", on_clear);
  native_hooks::on(kClearTasksImmediately, "CLEAR_CHAR_TASKS_IMMEDIATELY", on_clear);
}

// Turns a captured call (slot 0, the ped, skipped) into message arguments; false if an entity argument is
// not one we replicate.
bool to_args(const Captured& cap, const Spec& spec, sync::EntitySync& ents, std::vector<Arg>& out) {
  for (std::size_t i = 1; i < cap.slots.size(); ++i) {
    Arg a;
    if (spec.strings >> i & 1) {
      a.tag = Tag::String, a.text = make_key(cap.strings[i]);
    } else if (static_cast<int>(i) == spec.entity) {
      const auto other = ents.id_for_handle(cap.slots[i]);
      if (!other) return false; // an object we don't replicate: nothing to sit on over there
      a.tag = Tag::Entity, a.value = *other;
    } else {
      a.value = cap.slots[i];
    }
    out.push_back(a);
  }
  return true;
}

void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents) {
  g_hosting = hosting;
  if (!hosting) {
    g_pending.clear();
    g_playing.clear();
    g_open.clear();
    g_recipes.clear();
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
    if (const auto* perf = std::get_if<PerformSeq>(&item)) {
      const auto id = ents.id_for_handle(perf->ped);
      if (!id) continue;
      std::vector<Step> steps;
      std::size_t bytes = 8;
      for (const Captured& cap : perf->steps) {
        const Spec* spec = step_spec_for(cap.hash);
        Step st;
        st.hash = cap.hash;
        if (!spec || !to_args(cap, *spec, ents, st.args)) continue; // a step we can't express is skipped
        bytes += 5 + st.args.size() * 5 + std::count_if(st.args.begin(), st.args.end(), [](const Arg& a) { return a.tag == Tag::String; }) * 32;
        if (bytes > kMaxSequenceBytes) break;
        steps.push_back(std::move(st));
      }
      if (steps.empty()) continue;
      Playing p;
      p.msg = encode_sequence(*id, static_cast<std::uint8_t>(std::min(perf->first, 255)), steps);
      p.watch = p.seq = true;
      p.since = GetTickCount();
      net.send_game(p.msg, true);
      g_playing[*id] = std::move(p);
      continue;
    }
    const auto& cap = std::get<Captured>(item);
    const auto id = ents.id_for_handle(cap.ped);
    const Spec* spec = spec_for(cap.hash);
    if (!id || !spec) continue; // not a networked ped (or already gone)
    Start s;
    s.entity = *id, s.hash = cap.hash;
    if (!to_args(cap, *spec, ents, s.args)) continue;
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
      bool active;
      if (p.seq) {
        std::int32_t progress = -1;
        n::invoke(kGetSequenceProgress, ped, &progress);
        active = progress >= 0;
      } else {
        active = n::invoke<bool>(kIsCharPlayingAnim, ped, p.dict.c_str(), p.anim.c_str());
      }
      p.misses = active ? 0 : p.misses + 1;
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
  if (op == Op::Start || op == Op::Sequence) {
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
  g_open.clear();
  g_recipes.clear();
  g_entries.clear();
  for (const std::string& dict : g_dicts) n::invoke(kRemoveAnims, dict.c_str());
  g_dicts.clear();
}

} // namespace ivc::story::npc_anims
