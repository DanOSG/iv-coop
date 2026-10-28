#include "sync/attach_sync.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "core/log.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "net/bytes.h"
#include "story/story_sync.h"

namespace ivc::attach_sync {
namespace {
namespace n = natives;
using native_hooks::Call;

constexpr int kMaxArgs = 16;
constexpr DWORD kPendingTimeoutMs = 15000;
enum class Op : std::uint8_t { Attach = 1, Detach = 2 };
enum class RefKind : std::uint8_t { Entity = 0, HostPlayer = 1 };

struct Ref {
  RefKind kind = RefKind::Entity;
  std::uint32_t id = 0;
};

// A script call as captured: the child is argument 0, the parent argument 1 (attach).
struct Captured {
  Op op;
  std::uint32_t hash;
  int argc;
  std::uint32_t args[kMaxArgs];
};

struct Attachment { // decoded message
  Op op = Op::Attach;
  std::uint32_t hash = 0;
  int argc = 0;
  Ref child, parent;
  std::uint32_t args[kMaxArgs] = {};
};

bool g_hosting = false;
std::deque<Captured> g_captured;
std::map<std::uint32_t, std::vector<std::uint8_t>> g_host_attached; // child entity id -> its attach message

struct Pending {
  Attachment a;
  DWORD since = GetTickCount();
};
std::deque<Pending> g_pending;
std::map<std::uint32_t, std::vector<std::uint8_t>> g_applied; // child entity id -> attach message applied

void capture(const Call& c, Op op) {
  if (!g_hosting || !story::is_story_script(c.script) || c.arg_count < (op == Op::Attach ? 2 : 1)) return;
  Captured cap{op, c.hash, std::min(c.arg_count, kMaxArgs), {}};
  std::memcpy(cap.args, c.args, static_cast<std::size_t>(cap.argc) * 4);
  g_captured.push_back(cap);
}

std::vector<std::uint8_t> encode(const Attachment& a) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(a.op).put(a.hash).put(static_cast<std::uint8_t>(a.argc));
  w.put(a.child.kind).put(a.child.id).put(a.parent.kind).put(a.parent.id);
  for (int i = 0; i < a.argc; ++i) w.put(a.args[i]);
  return std::move(w.data());
}

std::optional<Attachment> decode(std::span<const std::uint8_t> d) {
  net::Reader r(d);
  Attachment a;
  std::uint8_t type, sender, argc = 0;
  r.get(type), r.get(sender), r.get(a.op), r.get(a.hash), r.get(argc);
  r.get(a.child.kind), r.get(a.child.id), r.get(a.parent.kind), r.get(a.parent.id);
  if (!r.ok() || argc > kMaxArgs || (a.op != Op::Attach && a.op != Op::Detach)) return std::nullopt;
  a.argc = argc;
  for (int i = 0; i < argc; ++i) r.get(a.args[i]);
  if (!r.ok()) return std::nullopt;
  return a;
}

// Same message, ignoring the sender byte (resent attachments are identical to the first send).
bool same_message(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
  return a.size() == b.size() && a.size() >= 2 && std::equal(a.begin() + 2, a.end(), b.begin() + 2);
}

void hook_attach(const char* name, std::uint32_t hash) {
  native_hooks::on(hash, name, [](const Call& c) { capture(c, Op::Attach); });
}
void hook_detach(const char* name, std::uint32_t hash) {
  native_hooks::on(hash, name, [](const Call& c) { capture(c, Op::Detach); });
}

} // namespace

void register_hooks() {
  hook_attach("ATTACH_OBJECT_TO_PED", 0x577A699E);
  hook_attach("ATTACH_OBJECT_TO_PED_PHYSICALLY", 0x1F760E1A);
  hook_attach("ATTACH_OBJECT_TO_CAR", 0x7E81412A);
  hook_attach("ATTACH_OBJECT_TO_CAR_PHYSICALLY", 0x161B05A9);
  hook_attach("ATTACH_OBJECT_TO_OBJECT", 0x089E42C1);
  hook_attach("ATTACH_OBJECT_TO_OBJECT_PHYSICALLY", 0x28663AA6);
  hook_attach("ATTACH_PED_TO_CAR", 0x3EFC1A7D);
  hook_attach("ATTACH_PED_TO_CAR_PHYSICALLY", 0x7FF3248C);
  hook_attach("ATTACH_PED_TO_OBJECT", 0x376917AB);
  hook_attach("ATTACH_PED_TO_OBJECT_PHYSICALLY", 0x782E78BF);
  hook_attach("ATTACH_CAR_TO_CAR", 0x64146142);
  hook_attach("ATTACH_CAR_TO_CAR_PHYSICALLY", 0x778F46E3);
  hook_attach("ATTACH_CAR_TO_OBJECT", 0x61C81E88);
  hook_attach("ATTACH_CAR_TO_OBJECT_PHYSICALLY", 0x32E57C15);
  hook_detach("DETACH_OBJECT", 0x05C87C26);
  hook_detach("DETACH_OBJECT_NO_COLLIDE", 0x6B2E49CD);
  hook_detach("DETACH_PED", 0x2CD52C5C);
  // DETACH_PED_FROM_WITHIN_CAR (0x15F70500) runs the same handler as DETACH_PED: that hook already sees its calls.
  hook_detach("DETACH_CAR", 0x34CC1F23);
}

// ---------------------------------------------------------------- host

void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents) {
  g_hosting = hosting;
  if (!hosting) {
    g_captured.clear();
    g_host_attached.clear();
    return;
  }
  const n::Ped player = n::GetPlayerChar(n::GetPlayerId());
  auto ref_of = [&](n::Handle h) -> std::optional<Ref> {
    if (!h) return std::nullopt;
    if (const auto id = ents.id_for_handle(h)) return Ref{RefKind::Entity, *id};
    if (h == player) return Ref{RefKind::HostPlayer, 0};
    return std::nullopt;
  };

  while (!g_captured.empty()) {
    const Captured cap = g_captured.front();
    g_captured.pop_front();
    Attachment a;
    a.op = cap.op, a.hash = cap.hash, a.argc = cap.argc;
    std::memcpy(a.args, cap.args, sizeof(a.args));
    const auto child = ref_of(static_cast<n::Handle>(cap.args[0]));
    // A child that is the host's player ped is not carried over: its copy is driven by the host's own stream.
    if (!child || child->kind != RefKind::Entity) continue;
    a.child = *child;
    if (cap.op == Op::Attach) {
      const auto parent = ref_of(static_cast<n::Handle>(cap.args[1]));
      if (!parent || (parent->kind == RefKind::Entity && parent->id == child->id)) continue; // not networked
      a.parent = *parent;
      auto bytes = encode(a);
      net.send_game(bytes, true);
      ents.set_attached(child->id, true);
      g_host_attached[child->id] = std::move(bytes);
      log::info("Attach: entity {} -> {} {} (native 0x{:08X}, {} args)", child->id,
                parent->kind == RefKind::HostPlayer ? "host player" : "entity", parent->id, cap.hash, cap.argc);
    } else if (g_host_attached.erase(child->id)) {
      net.send_game(encode(a), true);
      ents.set_attached(child->id, false);
      log::info("Detach: entity {} (native 0x{:08X})", child->id, cap.hash);
    }
  }

  // The script removed the child (or the parent's deletion took it) without a detach.
  for (auto it = g_host_attached.begin(); it != g_host_attached.end();) {
    if (!ents.handle_for_id(it->first))
      it = g_host_attached.erase(it);
    else
      ++it;
  }
}

void host_on_player_joined(net::Session& net) {
  for (const auto& [child, bytes] : g_host_attached) net.send_game(bytes, true);
  if (!g_host_attached.empty()) log::info("Sent {} attachments to a late joiner", g_host_attached.size());
}

// ---------------------------------------------------------------- joiner

void on_message(const net::GameMessage& m, sync::EntitySync& ents) {
  if (m.from != net::kHostId) return;
  const auto a = decode(m.data);
  if (!a || a->child.kind != RefKind::Entity) return;
  if (a->op == Op::Attach) {
    const auto it = g_applied.find(a->child.id);
    if (it != g_applied.end() && same_message(it->second, m.data)) return; // already attached this way
    ents.set_attached(a->child.id, true); // stop posing it right away, even if the copy isn't spawned yet
  }
  g_pending.push_back({*a, GetTickCount()});
}

void client_tick(sync::EntitySync& ents) {
  if (g_pending.empty()) return;
  const DWORD now = GetTickCount();
  auto handle_of = [&](const Ref& r) -> n::Handle {
    if (r.kind == RefKind::HostPlayer) {
      const n::Ped p = ents.player_ped ? ents.player_ped(net::kHostId) : 0;
      return p && n::DoesCharExist(p) ? p : 0;
    }
    return ents.handle_for_id(r.id);
  };

  std::set<std::uint32_t> blocked; // children with an earlier op still waiting: keep their order
  for (auto it = g_pending.begin(); it != g_pending.end();) {
    const Attachment& a = it->a;
    if (blocked.count(a.child.id)) {
      ++it;
      continue;
    }
    const n::Handle child = handle_of(a.child);
    const n::Handle parent = a.op == Op::Attach ? handle_of(a.parent) : 0;
    if (!child || (a.op == Op::Attach && !parent)) {
      if (a.op == Op::Detach && !child) { // nothing to detach: just let the stream have it
        ents.set_attached(a.child.id, false);
        g_applied.erase(a.child.id);
        it = g_pending.erase(it);
      } else if (now - it->since > kPendingTimeoutMs) {
        log::warn("Attach: gave up on entity {}: a copy never appeared", a.child.id);
        ents.set_attached(a.child.id, false);
        it = g_pending.erase(it);
      } else {
        blocked.insert(a.child.id);
        ++it;
      }
      continue;
    }

    std::uint32_t args[kMaxArgs];
    std::memcpy(args, a.args, sizeof(args));
    args[0] = static_cast<std::uint32_t>(child);
    if (a.op == Op::Attach) args[1] = static_cast<std::uint32_t>(parent);
    if (a.op == Op::Attach) ents.set_attached(a.child.id, true); // unfreezes an object copy
    n::invoke_raw(a.hash, args, a.argc);
    if (a.op == Op::Attach) {
      g_applied[a.child.id] = encode(a);
      log::info("Attach: entity {} attached to {} {} (native 0x{:08X})", a.child.id,
                a.parent.kind == RefKind::HostPlayer ? "host player" : "entity", a.parent.id, a.hash);
    } else {
      g_applied.erase(a.child.id);
      ents.set_attached(a.child.id, false);
      log::info("Detach: entity {} detached (native 0x{:08X})", a.child.id, a.hash);
    }
    it = g_pending.erase(it);
  }
}

void clear() {
  g_captured.clear();
  g_host_attached.clear();
  g_pending.clear();
  g_applied.clear();
}

} // namespace ivc::attach_sync
