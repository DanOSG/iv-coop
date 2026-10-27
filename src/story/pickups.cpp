#include "story/pickups.h"

#include <windows.h>

#include <deque>
#include <map>
#include <set>

#include "core/log.h"
#include "game/native_hooks.h"
#include "net/bytes.h"
#include "story/story_sync.h"

namespace ivc::story::pickups {
namespace {
namespace n = natives;
using native_hooks::Call;

enum class Op : std::uint8_t { Create = 0, Remove = 1, Collected = 2 };
enum class Kind : std::uint8_t { Plain, Rotate, Ammo, Money };

struct Spec {
  std::uint32_t id = 0; // the host's pickup handle
  Kind kind{};
  std::uint32_t model = 0, type = 0;
  std::uint32_t extra = 0; // Rotate: unknown; Ammo: ammo; Money: amount
  std::uint8_t flag = 0;
  float x = 0, y = 0, z = 0, rx = 0, ry = 0, rz = 0;
};

constexpr std::uint32_t kDoesExist = 0x7B567F1A, kHasCollected = 0x2F2226E5, kRemove = 0x2119007F;

std::vector<std::uint8_t> encode(Op op, const Spec& s) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(op).put(s);
  return std::move(w.data());
}

std::vector<std::uint8_t> encode_id(Op op, std::uint32_t id) {
  Spec s;
  s.id = id;
  return encode(op, s);
}

std::deque<std::vector<std::uint8_t>> g_out; // sent (and sent only) from tick

// ---------------------------------------------------------------- host

bool g_hosting = false;
struct HostPickup {
  Spec spec;
  bool polled = false; // the script asks whether it was collected: mission-critical
};
std::map<std::uint32_t, HostPickup> g_host;
std::set<std::uint32_t> g_collected; // a joiner collected it; the host's script is told so

bool capture(const Call& c) { return g_hosting && is_story_script(c.script); }

void created(const Call& c, Kind kind, int out_arg) {
  if (!capture(c)) return;
  Spec s;
  s.kind = kind;
  s.id = static_cast<std::uint32_t>(c.out<std::int32_t>(out_arg));
  if (!s.id) return;
  switch (kind) {
    case Kind::Plain:
      s.model = c.arg<std::uint32_t>(0), s.type = c.arg<std::uint32_t>(1);
      s.x = c.arg<float>(2), s.y = c.arg<float>(3), s.z = c.arg<float>(4), s.flag = c.arg<std::uint32_t>(6) != 0;
      break;
    case Kind::Rotate:
      s.model = c.arg<std::uint32_t>(0), s.type = c.arg<std::uint32_t>(1), s.extra = c.arg<std::uint32_t>(2);
      s.x = c.arg<float>(3), s.y = c.arg<float>(4), s.z = c.arg<float>(5);
      s.rx = c.arg<float>(6), s.ry = c.arg<float>(7), s.rz = c.arg<float>(8);
      break;
    case Kind::Ammo:
      s.model = c.arg<std::uint32_t>(0), s.type = c.arg<std::uint32_t>(1), s.extra = c.arg<std::uint32_t>(2);
      s.x = c.arg<float>(3), s.y = c.arg<float>(4), s.z = c.arg<float>(5);
      break;
    case Kind::Money:
      s.x = c.arg<float>(0), s.y = c.arg<float>(1), s.z = c.arg<float>(2);
      s.extra = c.arg<std::uint32_t>(3), s.flag = c.arg<std::uint32_t>(4) != 0;
      break;
  }
  g_collected.erase(s.id);
  g_host[s.id] = HostPickup{s, false};
  g_out.push_back(encode(Op::Create, s));
}

void removed(const Call& c) { // before the native runs
  if (!capture(c)) return;
  const auto id = c.arg<std::uint32_t>(0);
  g_collected.erase(id);
  if (g_host.erase(id)) g_out.push_back(encode_id(Op::Remove, id));
}

void has_collected(const Call& c) {
  if (!capture(c)) return;
  const auto id = c.arg<std::uint32_t>(0);
  if (g_collected.count(id)) {
    *reinterpret_cast<std::int32_t*>(c.ctx->result) = 1;
    return;
  }
  const auto it = g_host.find(id);
  if (it == g_host.end()) return;
  it->second.polled = true;
  if (c.result<std::int32_t>() & 1) { // the host collected it
    g_host.erase(it);
    g_out.push_back(encode_id(Op::Remove, id));
  }
}

void does_exist(const Call& c) {
  if (!capture(c)) return;
  if (g_collected.count(c.arg<std::uint32_t>(0))) *reinterpret_cast<std::int32_t*>(c.ctx->result) = 1;
}

// ---------------------------------------------------------------- joiner

struct Local {
  Spec spec;
  n::Handle handle = 0; // 0: waiting for the model (or nothing to show)
  bool done = false;    // created (or given up on)
  bool reported = false; // told the host we collected it
};
std::map<std::uint32_t, Local> g_local;

bool same_spec(const Spec& a, const Spec& b) {
  return a.kind == b.kind && a.model == b.model && a.x == b.x && a.y == b.y && a.z == b.z;
}

void remove_local(Local& l) {
  if (l.handle && n::invoke<bool>(kDoesExist, l.handle)) n::invoke(kRemove, l.handle);
  l.handle = 0;
}

void spawn(Local& l) {
  const Spec& s = l.spec;
  n::Handle h = 0;
  if (s.kind == Kind::Money) {
    n::invoke(0x19A0068, s.x, s.y, s.z, s.extra, static_cast<std::uint32_t>(s.flag), &h); // CREATE_MONEY_PICKUP
  } else {
    if (!n::IsModelInCdimage(s.model)) {
      l.done = true; // nothing to show
      return;
    }
    n::RequestModel(s.model);
    if (!n::HasModelLoaded(s.model)) return;
    if (s.kind == Kind::Plain)
      n::invoke(0x7E2868D4, s.model, s.type, s.x, s.y, s.z, &h, static_cast<std::uint32_t>(s.flag));
    else if (s.kind == Kind::Rotate)
      n::invoke(0x675E5940, s.model, s.type, s.extra, s.x, s.y, s.z, s.rx, s.ry, s.rz, &h);
    else
      n::invoke(0x1F736F00, s.model, s.type, s.extra, s.x, s.y, s.z, &h);
    n::MarkModelAsNoLongerNeeded(s.model);
  }
  l.handle = h;
  l.done = true;
}

} // namespace

void register_hooks() {
  using native_hooks::on;
  using native_hooks::before;
  on(0x7E2868D4, "CREATE_PICKUP", [](const Call& c) { created(c, Kind::Plain, 5); });
  on(0x675E5940, "CREATE_PICKUP_ROTATE", [](const Call& c) { created(c, Kind::Rotate, 9); });
  on(0x1F736F00, "CREATE_PICKUP_WITH_AMMO", [](const Call& c) { created(c, Kind::Ammo, 6); });
  on(0x19A0068, "CREATE_MONEY_PICKUP", [](const Call& c) { created(c, Kind::Money, 5); });
  before(kRemove, "REMOVE_PICKUP", removed);
  on(kHasCollected, "HAS_PICKUP_BEEN_COLLECTED", has_collected);
  on(kDoesExist, "DOES_PICKUP_EXIST", does_exist);
}

void tick(bool hosting, net::Session& net) {
  g_hosting = hosting;
  if (hosting) {
    // A pickup that vanished was collected by the host (or removed behind the script's back): joiners lose theirs.
    for (auto it = g_host.begin(); it != g_host.end();) {
      if (!n::invoke<bool>(kDoesExist, static_cast<n::Handle>(it->first))) {
        g_out.push_back(encode_id(Op::Remove, it->first));
        it = g_host.erase(it);
      } else {
        ++it;
      }
    }
  } else {
    g_host.clear(), g_collected.clear();
    for (auto& [id, l] : g_local) {
      if (!l.done) {
        spawn(l);
        continue;
      }
      if (!l.handle || l.reported) continue;
      if (!n::invoke<bool>(kDoesExist, l.handle) || n::invoke<bool>(kHasCollected, l.handle)) { // collected here
        remove_local(l);
        l.reported = true;
        g_out.push_back(encode_id(Op::Collected, id));
      }
    }
  }
  while (!g_out.empty()) {
    net.send_game(std::move(g_out.front()), true);
    g_out.pop_front();
  }
}

void on_message(const net::GameMessage& m, bool hosting) {
  net::Reader r(m.data);
  std::uint8_t type, sender;
  Op op;
  Spec s;
  if (!r.get(type) || !r.get(sender) || !r.get(op) || !r.get(s)) return;
  if (hosting) {
    if (op != Op::Collected) return;
    const auto it = g_host.find(s.id);
    if (it == g_host.end() || !it->second.polled) return; // per-machine pickup: their own effect only
    log::info("Story: pickup {} collected by player #{}; counted for the host's script", s.id, m.from);
    g_collected.insert(s.id);
    n::invoke(kRemove, static_cast<n::Handle>(s.id)); // the host's copy is gone too
    g_host.erase(it);
    g_out.push_back(encode_id(Op::Remove, s.id));
    return;
  }
  if (m.from != net::kHostId) return;
  if (op == Op::Create) {
    if (const auto it = g_local.find(s.id); it != g_local.end()) {
      if (same_spec(it->second.spec, s)) return; // a late-join resend
      remove_local(it->second);
    }
    g_local[s.id] = Local{s};
  } else if (op == Op::Remove) {
    if (const auto it = g_local.find(s.id); it != g_local.end()) {
      remove_local(it->second);
      g_local.erase(it);
    }
  }
}

void host_on_player_joined(std::uint8_t player, net::Session& net) {
  for (const auto& [id, p] : g_host) net.send_game(encode(Op::Create, p.spec), true);
  log::info("Story: sent {} pickups to late joiner #{}", g_host.size(), player);
}

n::Handle local_handle(std::uint32_t id) {
  const auto it = g_local.find(id);
  return it != g_local.end() ? it->second.handle : 0;
}

void clear() {
  for (auto& [id, l] : g_local) remove_local(l);
  g_local.clear(), g_host.clear(), g_collected.clear(), g_out.clear();
  g_hosting = false;
}

} // namespace ivc::story::pickups
