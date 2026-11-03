#include "sync/player_mirror.h"

#include <windows.h>

#include <string>
#include <vector>

#include "core/log.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "net/bytes.h"
#include "story/story_sync.h"

namespace ivc::player_mirror {
namespace {
namespace n = natives;
using native_hooks::Call;

enum Op : std::uint8_t {
  OpGive = 0, OpRemove, OpRemoveAll, OpArmour, OpVest, OpHealth, OpInvisibleToAi, OpNoJump, OpNoCrouch
};

constexpr std::uint32_t kGiveWeapon = 0x03E90416, kRemoveWeapon = 0x2485231E, kRemoveAllWeapons = 0x6BA520F0,
                        kAddArmour = 0x1C623537, kSetVest = 0x076A7E4E, kSetHealth = 0x575E2880,
                        kDisableJump = 0x4B5832BE, kDisableCrouch = 0x3BB57426;

struct Pending {
  Op op;
  std::uint32_t a, b;
};
bool g_hosting = false;
std::vector<Pending> g_out;

n::Ped player_ped() {
  const n::Ped p = n::GetPlayerChar(n::GetPlayerId());
  return p && n::DoesCharExist(p) ? p : 0;
}

// A story script's call on the host's player ped (arg 0) / player index (arg 0).
bool from_story(const Call& c) { return g_hosting && story::replicating(c.script) && g_out.size() < 64; }
bool on_my_ped(const Call& c) {
  const n::Ped me = player_ped();
  return me && c.arg<n::Ped>(0) == me;
}

void add(Op op, std::uint32_t a, std::uint32_t b = 0) { g_out.push_back({op, a, b}); }

} // namespace

void register_hooks() {
  using native_hooks::on;
  on(kGiveWeapon, "GIVE_WEAPON_TO_CHAR", [](const Call& c) {
    if (from_story(c) && on_my_ped(c)) add(OpGive, c.arg<std::uint32_t>(1), c.arg<std::uint32_t>(2));
  });
  on(kRemoveWeapon, "REMOVE_WEAPON_FROM_CHAR", [](const Call& c) {
    if (from_story(c) && on_my_ped(c)) add(OpRemove, c.arg<std::uint32_t>(1));
  });
  on(kRemoveAllWeapons, "REMOVE_ALL_CHAR_WEAPONS", [](const Call& c) {
    if (from_story(c) && on_my_ped(c)) add(OpRemoveAll, 0);
  });
  on(kAddArmour, "ADD_ARMOUR_TO_CHAR", [](const Call& c) {
    if (from_story(c) && on_my_ped(c)) add(OpArmour, c.arg<std::uint32_t>(1));
  });
  on(kSetVest, "SET_CHAR_BULLETPROOF_VEST", [](const Call& c) {
    if (from_story(c) && on_my_ped(c)) add(OpVest, c.arg<std::uint32_t>(1) != 0);
  });
  on(kSetHealth, "SET_CHAR_HEALTH", [](const Call& c) {
    if (from_story(c) && on_my_ped(c)) add(OpHealth, c.arg<std::uint32_t>(1));
  });
  on(kDisableJump, "SET_PLAYER_DISABLE_JUMP", [](const Call& c) {
    if (from_story(c) && c.arg<std::int32_t>(0) == n::GetPlayerId()) add(OpNoJump, c.arg<std::uint32_t>(1) != 0);
  });
  on(kDisableCrouch, "SET_PLAYER_DISABLE_CROUCH", [](const Call& c) {
    if (from_story(c) && c.arg<std::int32_t>(0) == n::GetPlayerId()) add(OpNoCrouch, c.arg<std::uint32_t>(1) != 0);
  });
}

namespace {

// Joiner: ops wait until our character has kept the same ped and model for kSettleMs. A weapon given in the frames
// right after client mode swapped our model crashed the game (the weapon's streaming job still pointed at the
// old ped).
constexpr DWORD kSettleMs = 750;
std::vector<Pending> g_in;
n::Ped g_seen_ped = 0;
std::uint32_t g_seen_model = 0;
DWORD g_stable_since = 0;

void apply(const Pending& p, n::Ped me) {
  switch (p.op) {
    case OpGive: n::invoke(kGiveWeapon, me, p.a, p.b, 0); break;
    case OpRemove: n::invoke(kRemoveWeapon, me, p.a); break;
    case OpRemoveAll: n::invoke(kRemoveAllWeapons, me); break;
    case OpArmour: n::invoke(kAddArmour, me, p.a); break;
    case OpVest: n::invoke(kSetVest, me, p.a != 0); break;
    case OpHealth: n::invoke(kSetHealth, me, p.a); break;
    case OpInvisibleToAi: return; // a no-op native in this version (its handler is an engine stub)
    case OpNoJump: n::invoke(kDisableJump, n::GetPlayerId(), p.a != 0); break;
    case OpNoCrouch: n::invoke(kDisableCrouch, n::GetPlayerId(), p.a != 0); break;
    default: return;
  }
  log::info("Player mirror: op {} ({}, {}) from the host's story", static_cast<int>(p.op), p.a, p.b);
}

void apply_settled() {
  const n::Ped me = player_ped();
  const DWORD now = GetTickCount();
  const std::uint32_t model = me ? n::GetCharModel(me) : 0;
  if (!me || me != g_seen_ped || model != g_seen_model) {
    g_seen_ped = me, g_seen_model = model, g_stable_since = now;
    return;
  }
  if (g_in.empty() || now - g_stable_since < kSettleMs) return;
  for (const Pending& p : g_in) apply(p, me);
  g_in.clear();
}

} // namespace

void tick(bool hosting, net::Session& net, bool ready) {
  g_hosting = hosting;
  if (!hosting) {
    g_out.clear();
    if (ready) apply_settled();
    return;
  }
  for (const Pending& p : g_out) {
    net::Writer w;
    w.put(kMsg).put(std::uint8_t{0}).put(static_cast<std::uint8_t>(p.op)).put(p.a).put(p.b);
    net.send_game(std::move(w.data()), true);
  }
  g_out.clear();
}

void on_message(const net::GameMessage& m) {
  if (m.from != net::kHostId) return;
  net::Reader r(m.data);
  std::uint8_t type, sender, op;
  std::uint32_t a, b;
  if (!r.get(type) || !r.get(sender) || !r.get(op) || !r.get(a) || !r.get(b) || op > OpNoCrouch) return;
  if (g_in.size() < 64) g_in.push_back({static_cast<Op>(op), a, b}); // applied by tick once our character is settled
}

} // namespace ivc::player_mirror
