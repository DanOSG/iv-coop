#include "story/world_state.h"

#include <format>
#include <map>
#include <string>

#include "core/log.h"
#include "game/native_hooks.h"
#include "game/script.h"
#include "net/bytes.h"
#include "story/story_protocol.h"
#include "story/story_sync.h"

namespace ivc::story::world {
namespace {
using native_hooks::Call;
using natives::invoke;

enum class Kind : std::uint8_t {
  RoadsOff, RoadsOn, RoadsBack, PedRoadsBack, // f[0..5] = box
  CarGen,                                     // i[0] = generator, i[1] = type
  Door,                                       // i[0] = model, f[0..2] = pos, i[1] = state, f[3]
  IplRequest, IplRemove,                      // name
  Emergency, MaxWanted, Trains, Boats, Planes, Garbage, // i[0]
  SaveHouse,                                  // i[0] = id, i[1] = enabled
  ZoneNoCops, ZonePopType, ZoneScum,          // name, i[0]
  CarGensArea,                                // f[0..5] = box, i[0]
  BuildingSwap,                               // f[0..3] = pos, radius; i[0] = from, i[1] = to
};

struct Op {
  Kind kind{};
  float f[6]{};
  std::int32_t i[3]{};
  Key name{};
};

// Natives (IV hashes, FusionFix list).
constexpr std::uint32_t kRoadsOn = 0x56553F38, kRoadsOff = 0x4C3C1F3C, kRoadsBack = 0x6251618F,
                        kPedRoadsBack = 0x6AA20B7E, kSwitchCarGen = 0x7CE83A30, kDoor = 0x10974B70,
                        kRequestIpl = 0x59FD4E83, kRemoveIpl = 0x787F38B5, kEmergency = 0x69A72C50,
                        kMaxWanted = 0x5D622498, kTrains = 0x0FFD1A92, kBoats = 0x7FC65855, kPlanes = 0x4E637988,
                        kGarbage = 0x060669FE, kSaveHouse = 0x208C03C9, kZoneNoCops = 0x64F37F05,
                        kZonePopType = 0x70582D53, kZoneScum = 0x5E5E4252, kCarGensArea = 0x69CE154F,
                        kBuildingSwap = 0x5E077484;

std::string box_key(const Op& o) {
  return std::format("{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f}", o.f[0], o.f[1], o.f[2], o.f[3], o.f[4], o.f[5]);
}

// Ops with the same key affect the same thing; the latest one wins.
std::string key_of(const Op& o) {
  switch (o.kind) {
    case Kind::RoadsOff: case Kind::RoadsOn: case Kind::RoadsBack: return "R" + box_key(o);
    case Kind::PedRoadsBack: return "P" + box_key(o);
    case Kind::CarGen: return std::format("G{}", o.i[0]);
    case Kind::Door: return std::format("D{:08X},{:.1f},{:.1f},{:.1f}", static_cast<std::uint32_t>(o.i[0]), o.f[0], o.f[1], o.f[2]);
    case Kind::IplRequest: case Kind::IplRemove: return "I" + to_string(o.name);
    case Kind::SaveHouse: return std::format("S{}", o.i[0]);
    case Kind::ZoneNoCops: case Kind::ZonePopType: case Kind::ZoneScum:
      return std::format("Z{}:{}", static_cast<int>(o.kind), to_string(o.name));
    case Kind::CarGensArea: return "A" + box_key(o);
    case Kind::BuildingSwap:
      return std::format("B{:.1f},{:.1f},{:.1f},{:08X}", o.f[0], o.f[1], o.f[2], static_cast<std::uint32_t>(o.i[0]));
    default: return std::format("F{}", static_cast<int>(o.kind)); // global flags
  }
}

void apply(const Op& o) {
  const auto box = [&](std::uint32_t h) { invoke(h, o.f[0], o.f[1], o.f[2], o.f[3], o.f[4], o.f[5]); };
  const std::string name = to_string(o.name);
  switch (o.kind) {
    case Kind::RoadsOff: box(kRoadsOff); break;
    case Kind::RoadsOn: box(kRoadsOn); break;
    case Kind::RoadsBack: box(kRoadsBack); break;
    case Kind::PedRoadsBack: box(kPedRoadsBack); break;
    case Kind::CarGen: invoke(kSwitchCarGen, o.i[0], o.i[1]); break;
    case Kind::Door: invoke(kDoor, static_cast<std::uint32_t>(o.i[0]), o.f[0], o.f[1], o.f[2], o.i[1], o.f[3]); break;
    case Kind::IplRequest: invoke(kRequestIpl, name.c_str()); break;
    case Kind::IplRemove: invoke(kRemoveIpl, name.c_str()); break;
    case Kind::Emergency: invoke(kEmergency, o.i[0] != 0); break;
    case Kind::MaxWanted: invoke(kMaxWanted, static_cast<std::uint32_t>(o.i[0])); break;
    case Kind::Trains: invoke(kTrains, o.i[0] != 0); break;
    case Kind::Boats: invoke(kBoats, o.i[0] != 0); break;
    case Kind::Planes: invoke(kPlanes, o.i[0] != 0); break;
    case Kind::Garbage: invoke(kGarbage, o.i[0] != 0); break;
    case Kind::SaveHouse: invoke(kSaveHouse, static_cast<std::uint32_t>(o.i[0]), o.i[1] != 0); break;
    case Kind::ZoneNoCops: invoke(kZoneNoCops, name.c_str(), o.i[0] != 0); break;
    case Kind::ZonePopType: invoke(kZonePopType, name.c_str(), o.i[0]); break;
    case Kind::ZoneScum: invoke(kZoneScum, name.c_str(), o.i[0]); break;
    case Kind::CarGensArea:
      invoke(kCarGensArea, o.f[0], o.f[1], o.f[2], o.f[3], o.f[4], o.f[5], o.i[0] != 0);
      break;
    case Kind::BuildingSwap:
      invoke(kBuildingSwap, o.f[0], o.f[1], o.f[2], o.f[3], static_cast<std::uint32_t>(o.i[0]),
             static_cast<std::uint32_t>(o.i[1]));
      break;
  }
}

// Best-effort inverse, back to the game's default. Kinds with no known default (a car
// generator's or door's original state, zone settings, safehouses) are left as they are;
// the other side's ledger sets them anyway when both touch the same ones.
void undo(const Op& o) {
  Op u = o;
  switch (o.kind) {
    case Kind::RoadsOff: case Kind::RoadsOn: u.kind = Kind::RoadsBack; break;
    case Kind::IplRequest: u.kind = Kind::IplRemove; break;
    case Kind::IplRemove: u.kind = Kind::IplRequest; break;
    case Kind::Emergency: case Kind::Trains: case Kind::Boats: case Kind::Planes: case Kind::Garbage:
      u.i[0] = 1;
      break;
    case Kind::MaxWanted: u.i[0] = 6; break;
    case Kind::BuildingSwap: std::swap(u.i[0], u.i[1]); break;
    default: return; // RoadsBack/PedRoadsBack are already defaults
  }
  apply(u);
}

std::vector<std::uint8_t> encode(const Op& o) {
  net::Writer w;
  w.put(StoryMsg::World).put(std::uint8_t{0}).put(o);
  return std::move(w.data());
}

struct Entry {
  Op op;
  std::uint32_t frame = 0; // world tick count when recorded
};
std::map<std::string, Entry> g_own; // our story scripts' world state
std::map<std::string, Op> g_host; // the host's, applied here while joined
bool g_own_undone = false;
std::vector<std::vector<std::uint8_t>> g_outbox;
std::size_t g_last_script_count = 0;
std::uint32_t g_frame = 0;

// A save load restarts every script and "main" re-applies the world *in the same frame*,
// before our tick (which runs after all scripts) can notice; so a reset keeps what was
// recorded this frame and drops only older entries.
void reset_own(const char* why) {
  const std::size_t before = g_own.size();
  std::erase_if(g_own, [](const auto& kv) { return kv.second.frame < g_frame; });
  if (before) log::info("World ledger: reset ({}): dropped {}, kept {} from this frame", why, before - g_own.size(), g_own.size());
}

// (main's thread id is reused across loads, so it can't signal a reload; see tick.)
void record(const Call& c, const Op& o) {
  if (!is_story_script(c.script)) return;
  g_own[key_of(o)] = Entry{o, g_frame};
  g_outbox.push_back(encode(o));
}

void on_box(std::uint32_t hash, const char* name, Kind kind) {
  native_hooks::on(hash, name, [kind](const Call& c) {
    Op o;
    o.kind = kind;
    for (int i = 0; i < 6; ++i) o.f[i] = c.arg<float>(i);
    record(c, o);
  });
}

void on_flag(std::uint32_t hash, const char* name, Kind kind) {
  native_hooks::on(hash, name, [kind](const Call& c) {
    Op o;
    o.kind = kind;
    o.i[0] = c.arg<std::int32_t>(0);
    record(c, o);
  });
}

void on_named(std::uint32_t hash, const char* name, Kind kind, bool with_value) {
  native_hooks::on(hash, name, [kind, with_value](const Call& c) {
    Op o;
    o.kind = kind;
    o.name = make_key(c.str(0));
    if (with_value) o.i[0] = c.arg<std::int32_t>(1);
    record(c, o);
  });
}

} // namespace

void register_hooks() {
  on_box(kRoadsOff, "SWITCH_ROADS_OFF", Kind::RoadsOff);
  on_box(kRoadsOn, "SWITCH_ROADS_ON", Kind::RoadsOn);
  on_box(kRoadsBack, "SWITCH_ROADS_BACK_TO_ORIGINAL", Kind::RoadsBack);
  on_box(kPedRoadsBack, "SWITCH_PED_ROADS_BACK_TO_ORIGINAL", Kind::PedRoadsBack);
  native_hooks::on(kSwitchCarGen, "SWITCH_CAR_GENERATOR", [](const Call& c) {
    Op o;
    o.kind = Kind::CarGen;
    o.i[0] = c.arg<std::int32_t>(0), o.i[1] = c.arg<std::int32_t>(1);
    record(c, o);
  });
  native_hooks::on(kDoor, "SET_STATE_OF_CLOSEST_DOOR_OF_TYPE", [](const Call& c) {
    Op o;
    o.kind = Kind::Door;
    o.i[0] = c.arg<std::int32_t>(0);
    o.f[0] = c.arg<float>(1), o.f[1] = c.arg<float>(2), o.f[2] = c.arg<float>(3);
    o.i[1] = c.arg<std::int32_t>(4), o.f[3] = c.arg<float>(5);
    record(c, o);
  });
  on_named(kRequestIpl, "REQUEST_IPL", Kind::IplRequest, false);
  on_named(kRemoveIpl, "REMOVE_IPL", Kind::IplRemove, false);
  on_named(0x658F21AF, "REMOVE_IPL_DISCREETLY", Kind::IplRemove, false);
  on_flag(kEmergency, "ALLOW_EMERGENCY_SERVICES", Kind::Emergency);
  on_flag(kMaxWanted, "SET_MAX_WANTED_LEVEL", Kind::MaxWanted);
  on_flag(kTrains, "SWITCH_RANDOM_TRAINS", Kind::Trains);
  on_flag(kBoats, "SWITCH_RANDOM_BOATS", Kind::Boats);
  on_flag(kPlanes, "SWITCH_AMBIENT_PLANES", Kind::Planes);
  on_flag(kGarbage, "SWITCH_GARBAGE_TRUCKS", Kind::Garbage);
  native_hooks::on(kSaveHouse, "ENABLE_SAVE_HOUSE", [](const Call& c) {
    Op o;
    o.kind = Kind::SaveHouse;
    o.i[0] = c.arg<std::int32_t>(0), o.i[1] = c.arg<std::int32_t>(1);
    record(c, o);
  });
  on_named(kZoneNoCops, "SET_ZONE_NO_COPS", Kind::ZoneNoCops, true);
  on_named(kZonePopType, "SET_ZONE_POPULATION_TYPE", Kind::ZonePopType, true);
  on_named(kZoneScum, "SET_ZONE_SCUMMINESS", Kind::ZoneScum, true);
  native_hooks::on(kCarGensArea, "SET_CAR_GENERATORS_ACTIVE_IN_AREA", [](const Call& c) {
    Op o;
    o.kind = Kind::CarGensArea;
    for (int i = 0; i < 6; ++i) o.f[i] = c.arg<float>(i);
    o.i[0] = c.arg<std::int32_t>(6);
    record(c, o);
  });
  native_hooks::on(kBuildingSwap, "SWAP_NEAREST_BUILDING_MODEL", [](const Call& c) {
    Op o;
    o.kind = Kind::BuildingSwap;
    for (int i = 0; i < 4; ++i) o.f[i] = c.arg<float>(i);
    o.i[0] = c.arg<std::int32_t>(4), o.i[1] = c.arg<std::int32_t>(5);
    record(c, o);
  });
}

void tick() {
  // A save load kills every script at once (seen as ~30 -> ~20 threads in one go).
  const std::size_t n = script::threads().size();
  if (g_last_script_count >= n + 8) reset_own("scripts restarted");
  g_last_script_count = n;
  ++g_frame;
}

std::vector<std::vector<std::uint8_t>> take_outbox() { return std::exchange(g_outbox, {}); }

std::vector<std::vector<std::uint8_t>> encode_ledger() {
  std::vector<std::vector<std::uint8_t>> out;
  for (const auto& [k, e] : g_own) out.push_back(encode(e.op));
  return out;
}

void client_apply(std::span<const std::uint8_t> msg) {
  net::Reader r(msg);
  StoryMsg type;
  std::uint8_t sender;
  Op o;
  if (!r.get(type) || !r.get(sender) || !r.get(o)) return;
  if (!g_own_undone) {
    for (auto it = g_own.rbegin(); it != g_own.rend(); ++it) undo(it->second.op);
    g_own_undone = true;
    log::info("World: undid our own {} world-state entries; applying the host's", g_own.size());
  }
  apply(o);
  g_host[key_of(o)] = o;
}

void client_restore() {
  if (!g_own_undone && g_host.empty()) return;
  for (auto it = g_host.rbegin(); it != g_host.rend(); ++it) undo(it->second);
  for (const auto& [k, e] : g_own) apply(e.op);
  log::info("World: undid {} host entries, restored our own {}", g_host.size(), g_own.size());
  g_host.clear();
  g_own_undone = false;
}

std::size_t own_size() { return g_own.size(); }

} // namespace ivc::story::world
