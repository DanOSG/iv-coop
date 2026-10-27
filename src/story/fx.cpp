#include "story/fx.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/native_hooks.h"
#include "net/bytes.h"
#include "story/story_sync.h"

namespace ivc::story::fx {
namespace {
namespace n = natives;
using native_hooks::Call;

constexpr int kMaxArgs = 16;
constexpr std::size_t kMaxPerFrame = 40, kMaxPending = 128, kMaxLive = 256;
constexpr DWORD kPendingMs = 4000;

constexpr std::uint32_t kStopPtfx = 0x0EAA4429, kRemoveScriptFire = 0x0E633C13, kRemoveAllScriptFires = 0x488CBB60;

enum class Cls : std::uint8_t { None = 0, Ptfx = 1, Fire = 2 };
enum RefType : std::uint8_t { kRefNone = 0, kRefEntity = 1, kRefHostPlayer = 2 };
enum Flags : std::uint8_t { kUnmap = 1, kUnmapAllFires = 2 };

using Key = std::pair<std::uint8_t, std::uint32_t>; // (class, host handle)

// One captured native call.
struct Record {
  std::uint32_t hash = 0;
  std::uint8_t flags = 0;
  Cls cls = Cls::None;           // handle namespace of host_result
  std::uint32_t host_result = 0; // handle the host's call returned (to map) or passed in map_slot
  std::int8_t name_slot = -1, ref_slot = -1, map_slot = -1;
  std::uint8_t ref_type = kRefNone;
  std::uint32_t ref_id = 0;
  char name[32] = {};
  std::uint8_t argc = 0;
  std::uint32_t args[kMaxArgs] = {};

  Key key() const { return {static_cast<std::uint8_t>(cls), host_result}; }
  bool starts_handle() const { return map_slot < 0 && host_result && cls != Cls::None; }
};

std::vector<std::uint8_t> encode(const Record& r) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(r.hash).put(r.flags).put(r.cls).put(r.host_result).put(r.name_slot)
      .put(r.ref_slot).put(r.map_slot).put(r.ref_type).put(r.ref_id);
  for (char ch : r.name) w.put(ch);
  w.put(r.argc);
  for (int i = 0; i < r.argc; ++i) w.put(r.args[i]);
  return std::move(w.data());
}

bool decode(std::span<const std::uint8_t> d, Record& r) {
  net::Reader rd(d);
  std::uint8_t type, sender;
  if (!(rd.get(type) && rd.get(sender) && rd.get(r.hash) && rd.get(r.flags) && rd.get(r.cls) &&
        rd.get(r.host_result) && rd.get(r.name_slot) && rd.get(r.ref_slot) && rd.get(r.map_slot) &&
        rd.get(r.ref_type) && rd.get(r.ref_id)))
    return false;
  for (char& ch : r.name)
    if (!rd.get(ch)) return false;
  if (!rd.get(r.argc) || r.argc > kMaxArgs) return false;
  for (int i = 0; i < r.argc; ++i)
    if (!rd.get(r.args[i])) return false;
  r.name[sizeof(r.name) - 1] = 0;
  return true;
}

// ---------------------------------------------------------------- host capture

bool g_active = false;
sync::EntitySync* g_ents = nullptr;
std::vector<std::vector<std::uint8_t>> g_out;
std::size_t g_frame_count = 0;
// Fires and looped particle effects still running, keyed (class, host handle): resent to late joiners.
std::map<Key, std::vector<std::uint8_t>> g_live;

struct Spec {
  const char* name;
  Cls cls = Cls::None;        // handle class (returned by the native, or taken from map_slot)
  bool returns = false;       // the native returns a handle to map
  std::int8_t name_slot = -1, ref_slot = -1, map_slot = -1;
  std::uint8_t flags = 0;
};

bool is_host_player(n::Handle h) {
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  return me && h == me;
}

void capture(const Call& c, const Spec& s) {
  if (!g_active || !g_ents || !story::replicating(c.script)) return;
  if (g_frame_count >= kMaxPerFrame) return;
  Record r;
  r.hash = c.hash, r.flags = s.flags, r.cls = s.cls, r.name_slot = s.name_slot, r.ref_slot = s.ref_slot,
  r.map_slot = s.map_slot;
  r.argc = static_cast<std::uint8_t>(std::clamp(c.arg_count, 0, kMaxArgs));
  for (int i = 0; i < r.argc; ++i) r.args[i] = c.args[i];
  if (s.name_slot >= 0) {
    const std::string nm = c.str(s.name_slot);
    std::memcpy(r.name, nm.data(), std::min(nm.size(), sizeof(r.name) - 1));
  }
  if (s.ref_slot >= 0) {
    const auto h = c.arg<n::Handle>(s.ref_slot);
    if (h && is_host_player(h)) {
      r.ref_type = kRefHostPlayer;
    } else if (const auto id = h ? g_ents->id_for_handle(h) : std::nullopt) {
      r.ref_type = kRefEntity, r.ref_id = *id;
    } else {
      return; // not a networked entity: nothing for joiners to attach to
    }
  }
  if (s.map_slot >= 0) r.host_result = c.arg<std::uint32_t>(s.map_slot);
  if (s.returns) {
    r.host_result = c.result<std::uint32_t>();
    if (!r.host_result) return; // the effect didn't start
  }
  auto bytes = encode(r);
  if (s.returns) {
    if (g_live.size() < kMaxLive) g_live[r.key()] = bytes;
  } else if (s.flags & kUnmap) {
    g_live.erase(r.key());
  }
  if (s.flags & kUnmapAllFires)
    std::erase_if(g_live, [](const auto& kv) { return kv.first.first == static_cast<std::uint8_t>(Cls::Fire); });
  g_out.push_back(std::move(bytes));
  ++g_frame_count;
}

void hook(std::uint32_t hash, const Spec& s) {
  native_hooks::on(hash, s.name, [s](const Call& c) { capture(c, s); });
}

// ---------------------------------------------------------------- joiner playback

struct Pending {
  Record r;
  DWORD since = GetTickCount();
};
std::deque<Pending> g_inbox;
std::map<Key, std::uint32_t> g_map; // (class, host handle) -> our handle

std::uint32_t call_raw(std::uint32_t hash, const std::uint32_t* args, int count) {
  n::CallContext ctx{};
  ctx.args = ctx.result = ctx.buf;
  std::memcpy(ctx.buf, args, static_cast<std::size_t>(count) * 4);
  ctx.arg_count = count;
  if (auto* fn = reinterpret_cast<void(__cdecl*)(n::CallContext*)>(n::handler(hash))) fn(&ctx);
  return *reinterpret_cast<std::uint32_t*>(ctx.buf);
}

enum class Result { Done, Wait, Drop };

Result apply(const Record& r, sync::EntitySync& ents, n::Ped host_ped) {
  if (r.starts_handle() && g_map.contains(r.key())) return Result::Done; // a late-join resend of one we have
  std::uint32_t args[kMaxArgs] = {};
  std::memcpy(args, r.args, sizeof(args));
  if (r.ref_slot >= 0 && r.ref_slot < r.argc) {
    n::Handle h = 0;
    if (r.ref_type == kRefEntity) h = ents.handle_for_id(r.ref_id);
    else if (r.ref_type == kRefHostPlayer && host_ped && n::DoesCharExist(host_ped)) h = host_ped;
    if (!h) return Result::Wait;
    args[r.ref_slot] = static_cast<std::uint32_t>(h);
  }
  if (r.map_slot >= 0 && r.map_slot < r.argc) {
    const auto it = g_map.find(r.key());
    if (it == g_map.end()) return Result::Drop; // never started here, or already gone
    args[r.map_slot] = it->second;
  }
  char name[sizeof(r.name)];
  std::memcpy(name, r.name, sizeof(name));
  if (r.name_slot >= 0 && r.name_slot < r.argc) args[r.name_slot] = reinterpret_cast<std::uint32_t>(name);
  const std::uint32_t result = call_raw(r.hash, args, r.argc);
  if (r.starts_handle()) {
    if (result) g_map[r.key()] = result;
  } else if ((r.flags & kUnmap) && r.map_slot >= 0) {
    g_map.erase(r.key());
  }
  if (r.flags & kUnmapAllFires)
    std::erase_if(g_map, [](const auto& kv) { return kv.first.first == static_cast<std::uint8_t>(Cls::Fire); });
  return Result::Done;
}

} // namespace

void register_hooks() {
  constexpr Cls P = Cls::Ptfx, F = Cls::Fire;
  // Explosions: raw argument slots (ADD_EXPLOSION_WITH_DIRECTION's signature isn't listed; its slots go as-is).
  hook(0x32DA5E3A, {"ADD_EXPLOSION"});
  hook(0x71855E7C, {"ADD_EXPLOSION_WITH_DIRECTION"});
  hook(0x505518A2, {"EXPLODE_CAR", Cls::None, false, -1, 0});
  hook(0x4A802E89, {"EXPLODE_CHAR_HEAD", Cls::None, false, -1, 0});
  // Fires. The returned fire id is mapped so REMOVE_SCRIPT_FIRE finds ours.
  hook(0x24742BB9, {"START_SCRIPT_FIRE", F, true});
  hook(0x94BC3DCE, {"START_CAR_FIRE", F, true, -1, 0});
  hook(0x5FB31295, {"START_CHAR_FIRE", F, true, -1, 0});
  hook(0x2D7D5DD2, {"START_OBJECT_FIRE", F, true, -1, 0});
  hook(kRemoveScriptFire, {"REMOVE_SCRIPT_FIRE", F, false, -1, -1, 0, kUnmap});
  hook(kRemoveAllScriptFires, {"REMOVE_ALL_SCRIPT_FIRES", F, false, -1, -1, -1, kUnmapAllFires});
  // Particle effects: (name, [entity,] offset xyz, rotation xyz, [bone,] flags). START_* loop and return a handle.
  hook(0x3A774777, {"START_PTFX", P, true, 0});
  hook(0x0D8407E9, {"START_PTFX_ON_OBJ", P, true, 0, 1});
  hook(0x60980323, {"START_PTFX_ON_OBJ_BONE", P, true, 0, 1});
  hook(0x381C1F1C, {"START_PTFX_ON_PED", P, true, 0, 1});
  hook(0x2209116C, {"START_PTFX_ON_PED_BONE", P, true, 0, 1});
  hook(0x5C4B1A8A, {"START_PTFX_ON_VEH", P, true, 0, 1});
  hook(0x21C44026, {"TRIGGER_PTFX", Cls::None, false, 0});
  hook(0x50307F63, {"TRIGGER_PTFX_ON_OBJ", Cls::None, false, 0, 1});
  hook(0x3A2A77F9, {"TRIGGER_PTFX_ON_OBJ_BONE", Cls::None, false, 0, 1});
  hook(0x0A76502F, {"TRIGGER_PTFX_ON_PED", Cls::None, false, 0, 1});
  hook(0x7D3C3C9D, {"TRIGGER_PTFX_ON_PED_BONE", Cls::None, false, 0, 1});
  hook(0x3C7B6092, {"TRIGGER_PTFX_ON_VEH", Cls::None, false, 0, 1});
  hook(kStopPtfx, {"STOP_PTFX", P, false, -1, -1, 0, kUnmap});
  hook(0x4AF643D5, {"REMOVE_PTFX", P, false, -1, -1, 0, kUnmap});
  hook(0x4D7775BA, {"REMOVE_PTFX_FROM_OBJECT", Cls::None, false, -1, 0});
  hook(0x2FC9782A, {"REMOVE_PTFX_FROM_PED", Cls::None, false, -1, 0});
  hook(0x3FB14EC5, {"REMOVE_PTFX_FROM_VEHICLE", Cls::None, false, -1, 0});
}

void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents) {
  g_active = hosting;
  g_ents = hosting ? &ents : nullptr;
  g_frame_count = 0;
  if (!hosting) {
    g_out.clear();
    g_live.clear();
    return;
  }
  for (auto& bytes : g_out) net.send_game(std::move(bytes), true);
  g_out.clear();
}

void host_on_player_joined(std::uint8_t player, net::Session& net) {
  for (const auto& [key, bytes] : g_live) net.send_game(bytes, true);
  log::info("Story fx: resent {} live fires/effects to late joiner #{}", g_live.size(), player);
}

void client_on_message(const net::GameMessage& m) {
  if (m.from != net::kHostId) return; // only the host tells the story
  Pending p;
  if (!decode(m.data, p.r)) return;
  if (g_inbox.size() >= kMaxPending) g_inbox.pop_front();
  g_inbox.push_back(p);
}

void client_tick(sync::EntitySync& ents, n::Ped host_ped) {
  if (g_inbox.empty()) return;
  const DWORD now = GetTickCount();
  std::deque<Pending> keep;
  std::set<Key> blocked; // handles whose start is still waiting: later records on them wait too
  bool fires_blocked = false;
  for (auto& p : g_inbox) {
    const Record& r = p.r;
    const bool behind = (r.map_slot >= 0 && blocked.contains(r.key())) ||
                        ((r.flags & kUnmapAllFires) && fires_blocked);
    Result res = behind ? Result::Wait : apply(r, ents, host_ped);
    if (res == Result::Wait && now - p.since > kPendingMs) res = Result::Drop;
    if (res == Result::Wait) {
      if (r.starts_handle()) {
        blocked.insert(r.key());
        if (r.cls == Cls::Fire) fires_blocked = true;
      }
      keep.push_back(std::move(p));
    }
  }
  g_inbox = std::move(keep);
}

void clear() {
  for (const auto& [key, local] : g_map) {
    const std::uint32_t a[1] = {local};
    if (key.first == static_cast<std::uint8_t>(Cls::Fire)) call_raw(kRemoveScriptFire, a, 1);
    else if (key.first == static_cast<std::uint8_t>(Cls::Ptfx)) call_raw(kStopPtfx, a, 1);
  }
  g_map.clear();
  g_inbox.clear();
  g_out.clear();
  g_live.clear();
  g_active = false;
  g_ents = nullptr;
}

} // namespace ivc::story::fx
