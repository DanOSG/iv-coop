#include "sync/vehicle_props.h"

#include <windows.h>

#include <algorithm>
#include <deque>
#include <map>
#include <tuple>
#include <optional>
#include <vector>

#include "core/log.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "net/bytes.h"
#include "story/story_sync.h"

namespace ivc::vehicle_props {
namespace {
namespace n = natives;
using native_hooks::Call;

enum Kind : std::uint8_t { kAppearance = 0, kState = 1, kOp = 2 };

enum Op : std::uint8_t {
  OpenDoor = 1, ShutDoor, ControlDoor, BreakDoor, CloseAllDoors, LockDoors, SmashWindow, BurstTyre, FixTyre, FixCar,
  Siren, ForceLights, LightsOn, Hazard, Indicator, Taxi, Livery, ExtraColours, Colour, Visible, ExtraOff,
};

struct Appearance {
  std::int16_t c1 = -1, c2 = -1, e1 = -1, e2 = -1, livery = -1;
  std::uint16_t extras = 0; // bit i: extra i is on (i = 1..9)
  std::uint8_t dirt = 0;
  bool operator==(const Appearance&) const = default;
};

struct State {
  std::uint8_t siren = 0, taxi = 0, tyres = 0, windows = 0, doors = 0; // tyres/windows/doors: burst/smashed/damaged
  bool operator==(const State&) const = default;
};

struct WireOp {
  std::uint8_t op = 0;
  std::int32_t a = 0, b = 0;
  float f = 0;
};

// Which earlier ops a new one replaces: same class and key. Class 0: not kept.
struct Slot {
  std::uint8_t cls;
  std::int32_t key;
};
Slot slot_of(const WireOp& o) {
  switch (o.op) {
    case OpenDoor: case ShutDoor: case ControlDoor: case BreakDoor: return {1, o.a};
    case CloseAllDoors: return {1, -1};
    case LockDoors: return {2, 0};
    case SmashWindow: return {3, o.a};
    case BurstTyre: case FixTyre: return {4, o.a};
    case Siren: return {5, 0};
    case ForceLights: return {6, 0};
    case LightsOn: return {7, 0};
    case Hazard: return {8, 0};
    case Indicator: return {9, 0};
    case Taxi: return {10, 0};
    case Livery: return {11, 0};
    case ExtraColours: return {12, 0};
    case Colour: return {13, 0};
    case Visible: return {14, 0};
    case ExtraOff: return {15, o.a};
    default: return {0, 0};
  }
}

// Everything we know about one networked vehicle (every machine; the host rebroadcasts it to joiners).
struct Entry {
  std::optional<Appearance> app;
  State state;
  std::vector<WireOp> ops; // in order, superseded ones dropped
  // Sender side: last values polled off our copy.
  std::optional<Appearance> polled_app;
  std::optional<State> polled_state;
  // Receiver side: the local copy the above was delivered to, and what is still to deliver to it.
  n::Handle applied = 0;
  State applied_state;
  bool app_dirty = false;
  std::vector<WireOp> todo;
  DWORD missing_since = 0;
};

bool g_hosting = false;
std::map<std::uint32_t, Entry> g_entries;
struct Pending {
  n::Handle h;
  WireOp op;
};
std::deque<Pending> g_pending; // host: script calls waiting for the next tick
// Scripts often repeat a call every frame (LOCK_CAR_DOORS, CONTROL_CAR_DOOR while animating a door): an op
// identical to the last one sent for that slot is dropped, and a changing one goes out at most every
// kOpMinMs, its latest value kept until then.
constexpr DWORD kOpMinMs = 200;
struct Throttle {
  WireOp sent{};
  DWORD sent_at = 0;
  std::optional<WireOp> waiting;
};
std::map<std::tuple<std::uint32_t, std::uint8_t, std::int32_t>, Throttle> g_throttle; // (id, op, a)
DWORD g_last_poll = 0;

void remember(Entry& e, const WireOp& o) {
  if (o.op == FixCar) { // back to factory: damage and door ops no longer apply
    std::erase_if(e.ops, [](const WireOp& x) { const auto c = slot_of(x).cls; return c == 1 || c == 3 || c == 4; });
    e.state = State{};
    return;
  }
  const Slot s = slot_of(o);
  if (!s.cls) return;
  if (s.cls == 1 && s.key == -1)
    std::erase_if(e.ops, [](const WireOp& x) { return slot_of(x).cls == 1; });
  else
    std::erase_if(e.ops, [&](const WireOp& x) { const Slot t = slot_of(x); return t.cls == s.cls && t.key == s.key; });
  e.ops.push_back(o);
}

// ---------------------------------------------------------------- wire

std::vector<std::uint8_t> encode(std::uint32_t id, const Appearance& a) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(std::uint8_t{kAppearance}).put(id);
  w.put(a.c1).put(a.c2).put(a.e1).put(a.e2).put(a.livery).put(a.extras).put(a.dirt);
  return std::move(w.data());
}

std::vector<std::uint8_t> encode(std::uint32_t id, const State& s) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(std::uint8_t{kState}).put(id);
  w.put(s.siren).put(s.taxi).put(s.tyres).put(s.windows).put(s.doors);
  return std::move(w.data());
}

std::vector<std::uint8_t> encode(std::uint32_t id, const WireOp& o) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(std::uint8_t{kOp}).put(id);
  w.put(o.op).put(o.a).put(o.b).put(o.f);
  return std::move(w.data());
}

// ---------------------------------------------------------------- the game side
// Natives can fault on an index a model doesn't have; a fault just ends that vehicle's read/write.

bool read_appearance(n::Vehicle v, Appearance* out) {
  __try {
    std::int32_t c1 = -1, c2 = -1, e1 = -1, e2 = -1, liv = -1;
    float dirt = 0;
    n::invoke(0x6CAC3D62, v, &c1, &c2);  // GET_CAR_COLOURS
    n::invoke(0x25B87BCA, v, &e1, &e2);  // GET_EXTRA_CAR_COLOURS
    n::invoke(0x10237666, v, &liv);      // GET_CAR_LIVERY
    n::invoke(0x571152F5, v, &dirt);     // GET_VEHICLE_DIRT_LEVEL
    out->c1 = static_cast<std::int16_t>(c1), out->c2 = static_cast<std::int16_t>(c2);
    out->e1 = static_cast<std::int16_t>(e1), out->e2 = static_cast<std::int16_t>(e2);
    out->livery = static_cast<std::int16_t>(liv);
    out->dirt = static_cast<std::uint8_t>(std::clamp(dirt + 0.5f, 0.0f, 15.0f));
    out->extras = 0;
    for (std::uint32_t i = 1; i <= 9; ++i)
      if (n::invoke<bool>(0x4B920E81, v, i)) out->extras |= static_cast<std::uint16_t>(1u << i);  // IS_VEHICLE_EXTRA_TURNED_ON
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Wheels 0,1,4,5 exist on every car; windows 0-3; doors 0-5.
constexpr std::uint32_t kTyres[] = {0, 1, 4, 5};

bool read_state(n::Vehicle v, State* out) {
  __try {
    State s;
    s.siren = n::invoke<bool>(0x129A1569, v);  // IS_CAR_SIREN_ON
    s.taxi = n::invoke<bool>(0x5F4B0B22, v);   // ARE_TAXI_LIGHTS_ON
    for (const std::uint32_t t : kTyres)
      if (n::invoke<bool>(0x1DF623F9, v, t)) s.tyres |= static_cast<std::uint8_t>(1u << t);  // IS_CAR_TYRE_BURST
    for (std::uint32_t i = 0; i < 4; ++i)
      if (!n::invoke<bool>(0x1D0B131A, v, i)) s.windows |= static_cast<std::uint8_t>(1u << i);  // IS_VEH_WINDOW_INTACT
    for (std::uint32_t i = 0; i < 6; ++i)
      if (n::invoke<bool>(0x5AFE791F, v, i)) s.doors |= static_cast<std::uint8_t>(1u << i);  // IS_CAR_DOOR_DAMAGED
    *out = s;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void apply_appearance(n::Vehicle v, const Appearance& a) {
  __try {
    std::int32_t c1 = -1, c2 = -1, e1 = -1, e2 = -1, liv = -1;
    float dirt = 0;
    n::invoke(0x6CAC3D62, v, &c1, &c2);
    n::invoke(0x25B87BCA, v, &e1, &e2);
    n::invoke(0x10237666, v, &liv);
    n::invoke(0x571152F5, v, &dirt);
    if (a.c1 >= 0 && a.c2 >= 0 && (c1 != a.c1 || c2 != a.c2))
      n::invoke(0x06441EAF, v, static_cast<std::int32_t>(a.c1), static_cast<std::int32_t>(a.c2));  // CHANGE_CAR_COLOUR
    if (a.e1 >= 0 && a.e2 >= 0 && (e1 != a.e1 || e2 != a.e2))
      n::invoke(0x6CB14354, v, static_cast<std::int32_t>(a.e1), static_cast<std::int32_t>(a.e2));  // SET_EXTRA_CAR_COLOURS
    if (a.livery >= 0 && liv != a.livery) n::invoke(0x2E9E149D, v, static_cast<std::int32_t>(a.livery));  // SET_CAR_LIVERY
    if (static_cast<int>(dirt + 0.5f) != a.dirt) n::invoke(0x02A57428, v, static_cast<float>(a.dirt));  // SET_VEHICLE_DIRT_LEVEL
    for (std::uint32_t i = 1; i <= 9; ++i) {
      const bool want = (a.extras >> i) & 1u, have = n::invoke<bool>(0x4B920E81, v, i);
      if (want != have)
        n::invoke(0x05966824, v, static_cast<std::int32_t>(i), static_cast<std::uint32_t>(!want));  // TURN_OFF_VEHICLE_EXTRA
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void apply_op(n::Vehicle v, const WireOp& o) {
  const auto a = static_cast<std::uint32_t>(o.a), b = static_cast<std::uint32_t>(o.b);
  __try {
    switch (o.op) {
      case OpenDoor: n::invoke(0x1E352CEF, v, a); break;
      case ShutDoor: n::invoke(0x5E7A620E, v, a); break;
      case ControlDoor: n::invoke(0x194F76D4, v, a, b, o.f); break;
      case BreakDoor: n::invoke(0x18BD071B, v, a, b); break;
      case CloseAllDoors: n::invoke(0x56B8674F, v); break;
      case LockDoors: n::invoke(0x6702757C, v, a); break;
      case SmashWindow: n::invoke(0x2CDF628C, v, o.a); break;
      case BurstTyre: n::invoke(0x690D344F, v, a); break;
      case FixTyre: n::invoke(0x0FDA7965, v, a); break;
      case FixCar: n::invoke(0x3D562F78, v); break;
      case Siren: n::invoke(0x7781290F, v, b); break;
      case ForceLights: n::invoke(0x71B81DE7, v, o.a); break;
      case LightsOn: n::invoke(0x22888C5A, v, b); break;
      case Hazard: n::invoke(0x24B42ED2, v, b); break;
      case Indicator: n::invoke(0x71D72486, v, b); break;
      case Taxi: n::invoke(0x460837F9, v, b); break;
      case Livery: n::invoke(0x2E9E149D, v, o.a); break;
      case ExtraColours: n::invoke(0x6CB14354, v, o.a, o.b); break;
      case Colour: n::invoke(0x06441EAF, v, o.a, o.b); break;
      case Visible: n::invoke(0x02D13D06, v, b); break;
      case ExtraOff: n::invoke(0x05966824, v, o.a, b); break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Delivers what changed between `prev` and `s`; damage only goes one way (tyres can be fixed).
void apply_state(n::Vehicle v, const State& prev, const State& s) {
  __try {
    if (s.siren != prev.siren) n::invoke(0x7781290F, v, static_cast<std::uint32_t>(s.siren));
    if (s.taxi != prev.taxi) n::invoke(0x460837F9, v, static_cast<std::uint32_t>(s.taxi));
    for (const std::uint32_t t : kTyres) {
      const auto bit = static_cast<std::uint8_t>(1u << t);
      if ((s.tyres ^ prev.tyres) & bit) n::invoke((s.tyres & bit) ? 0x690D344F : 0x0FDA7965, v, t);
    }
    for (std::uint32_t i = 0; i < 4; ++i)
      if ((s.windows & ~prev.windows) & (1u << i)) n::invoke(0x2CDF628C, v, static_cast<std::int32_t>(i));
    for (std::uint32_t i = 0; i < 6; ++i)
      if ((s.doors & ~prev.doors) & (1u << i)) n::invoke(0x18BD071B, v, i, std::uint32_t{0});
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// ---------------------------------------------------------------- capture

bool capture(const Call& c) { return g_hosting && story::is_story_script(c.script); }

// (veh [, a [, b]]): arg indices of the op's a and b, -1 for none; `fa`: float argument.
void queue(const Call& c, std::uint8_t op, int a = -1, int b = -1, int fa = -1, bool b_is_flag = false) {
  if (!capture(c)) return;
  Pending p;
  p.h = c.arg<n::Handle>(0);
  p.op.op = op;
  if (a >= 0) p.op.a = c.arg<std::int32_t>(a);
  if (b >= 0) p.op.b = b_is_flag ? c.arg<std::int32_t>(b) != 0 : c.arg<std::int32_t>(b);
  if (fa >= 0) p.op.f = c.arg<float>(fa);
  if (p.h) g_pending.push_back(p);
}

} // namespace

void register_hooks() {
  using native_hooks::on;
  on(0x1E352CEF, "OPEN_CAR_DOOR", [](const Call& c) { queue(c, OpenDoor, 1); });
  on(0x5E7A620E, "SHUT_CAR_DOOR", [](const Call& c) { queue(c, ShutDoor, 1); });
  on(0x194F76D4, "CONTROL_CAR_DOOR", [](const Call& c) { queue(c, ControlDoor, 1, 2, 3, true); });
  on(0x18BD071B, "BREAK_CAR_DOOR", [](const Call& c) { queue(c, BreakDoor, 1, 2, -1, true); });
  on(0x56B8674F, "CLOSE_ALL_CAR_DOORS", [](const Call& c) { queue(c, CloseAllDoors); });
  on(0x6702757C, "LOCK_CAR_DOORS", [](const Call& c) { queue(c, LockDoors, 1); });
  on(0x2CDF628C, "SMASH_CAR_WINDOW", [](const Call& c) { queue(c, SmashWindow, 1); });
  on(0x690D344F, "BURST_CAR_TYRE", [](const Call& c) { queue(c, BurstTyre, 1); });
  on(0x0FDA7965, "FIX_CAR_TYRE", [](const Call& c) { queue(c, FixTyre, 1); });
  on(0x3D562F78, "FIX_CAR", [](const Call& c) { queue(c, FixCar); });
  on(0x7781290F, "SWITCH_CAR_SIREN", [](const Call& c) { queue(c, Siren, -1, 1, -1, true); });
  on(0x71B81DE7, "FORCE_CAR_LIGHTS", [](const Call& c) { queue(c, ForceLights, 1); });
  on(0x22888C5A, "SET_CAR_LIGHTS_ON", [](const Call& c) { queue(c, LightsOn, -1, 1, -1, true); });
  on(0x24B42ED2, "SET_VEH_HAZARDLIGHTS", [](const Call& c) { queue(c, Hazard, -1, 1, -1, true); });
  on(0x71D72486, "SET_VEH_INDICATORLIGHTS", [](const Call& c) { queue(c, Indicator, -1, 1, -1, true); });
  on(0x460837F9, "SET_TAXI_LIGHTS", [](const Call& c) { queue(c, Taxi, -1, 1, -1, true); });
  on(0x2E9E149D, "SET_CAR_LIVERY", [](const Call& c) { queue(c, Livery, 1); });
  on(0x6CB14354, "SET_EXTRA_CAR_COLOURS", [](const Call& c) { queue(c, ExtraColours, 1, 2); });
  on(0x06441EAF, "CHANGE_CAR_COLOUR", [](const Call& c) { queue(c, Colour, 1, 2); });
  on(0x02D13D06, "SET_CAR_VISIBLE", [](const Call& c) { queue(c, Visible, -1, 1, -1, true); });
  on(0x05966824, "TURN_OFF_VEHICLE_EXTRA", [](const Call& c) { queue(c, ExtraOff, 1, 2, -1, true); });
}

void tick(bool in_session, bool hosting, sync::EntitySync& ents, net::Session& net) {
  g_hosting = in_session && hosting;
  if (!in_session) {
    g_pending.clear();
    return;
  }
  const DWORD now = GetTickCount();

  // Host: script calls captured since last frame (the vehicles were networked by story::host_tick).
  while (!g_pending.empty()) {
    const Pending p = g_pending.front();
    g_pending.pop_front();
    if (!hosting) continue;
    const auto id = ents.id_for_handle(p.h);
    if (!id) continue;
    Throttle& t = g_throttle[{*id, p.op.op, p.op.a}];
    if (t.sent_at && t.sent.b == p.op.b && t.sent.f == p.op.f) {
      t.waiting.reset();
      continue;
    }
    if (t.sent_at && now - t.sent_at < kOpMinMs) {
      t.waiting = p.op;
      continue;
    }
    t.sent = p.op, t.sent_at = now, t.waiting.reset();
    remember(g_entries[*id], p.op);
    net.send_game(encode(*id, p.op), true);
  }
  if (hosting)
    for (auto& [key, t] : g_throttle)
      if (t.waiting && now - t.sent_at >= kOpMinMs) {
        const std::uint32_t id = std::get<0>(key);
        t.sent = *t.waiting, t.sent_at = now, t.waiting.reset();
        remember(g_entries[id], t.sent);
        net.send_game(encode(id, t.sent), true);
      }

  // Poll at 4 Hz: appearance (host: it is the one who knows) and damage (whoever runs the live copy).
  const bool poll = now - g_last_poll >= 250;
  if (poll) g_last_poll = now;
  const auto status = ents.status();
  for (const auto& s : status) {
    if (s.type != sync::EntityType::Vehicle) continue;
    const n::Handle h = ents.handle_for_id(s.id);
    if (!h || !n::DoesVehicleExist(h)) continue;
    auto it = g_entries.find(s.id);
    if (it == g_entries.end()) {
      if (!poll || (!hosting && !s.live)) continue; // followers wait for the host's messages
      it = g_entries.try_emplace(s.id).first;
    }
    Entry& e = it->second;
    e.missing_since = 0;

    if (poll && hosting) {
      Appearance a;
      if (read_appearance(h, &a) && (!e.polled_app || a != *e.polled_app)) {
        e.polled_app = a, e.app = a;
        net.send_game(encode(s.id, a), true);
      }
    }
    if (poll && s.live) {
      State st;
      if (read_state(h, &st) && (!e.polled_state || st != *e.polled_state)) {
        // The first read is only a baseline when nothing is wrong with the vehicle yet.
        const bool first = !e.polled_state;
        e.polled_state = st, e.state = st;
        if (!first || st != State{}) net.send_game(encode(s.id, st), true);
      }
    }

    // Follower copy: deliver what arrived, or everything if the copy is new or was recreated.
    if (s.live) {
      e.applied = h, e.applied_state = e.state, e.app_dirty = false, e.todo.clear();
    } else if (e.applied != h) {
      e.applied = h;
      if (!hosting) { // the host's own copy already went through its scripts
        if (e.app) apply_appearance(h, *e.app);
        for (const auto& o : e.ops) apply_op(h, o);
        apply_state(h, State{}, e.state);
      } else {
        apply_state(h, State{}, e.state);
      }
      e.applied_state = e.state, e.app_dirty = false, e.todo.clear();
    } else {
      if (e.app_dirty && e.app && !hosting) apply_appearance(h, *e.app);
      e.app_dirty = false;
      for (const auto& o : e.todo) apply_op(h, o);
      e.todo.clear();
      if (e.state != e.applied_state) apply_state(h, e.applied_state, e.state);
      e.applied_state = e.state;
    }
  }

  // Forget vehicles that stayed gone.
  for (auto it = g_entries.begin(); it != g_entries.end();) {
    const bool present = std::any_of(status.begin(), status.end(), [&](const auto& s) { return s.id == it->first; });
    if (present) {
      ++it;
      continue;
    }
    if (!it->second.missing_since) it->second.missing_since = now ? now : 1;
    it = now - it->second.missing_since > 5000 ? g_entries.erase(it) : std::next(it);
  }
}

void on_message(const net::GameMessage& m) {
  net::Reader r(m.data);
  std::uint8_t type, sender, kind;
  std::uint32_t id;
  if (!r.get(type) || !r.get(sender) || !r.get(kind) || !r.get(id)) return;
  const bool from_host = m.from == net::kHostId;
  if (kind == kAppearance) {
    Appearance a;
    if (!from_host || !r.get(a.c1) || !r.get(a.c2) || !r.get(a.e1) || !r.get(a.e2) || !r.get(a.livery) ||
        !r.get(a.extras) || !r.get(a.dirt))
      return;
    Entry& e = g_entries[id];
    e.app = a, e.app_dirty = true;
  } else if (kind == kState) {
    State s;
    if (!r.get(s.siren) || !r.get(s.taxi) || !r.get(s.tyres) || !r.get(s.windows) || !r.get(s.doors)) return;
    g_entries[id].state = s;
  } else if (kind == kOp) {
    WireOp o;
    if (!from_host || !r.get(o.op) || !r.get(o.a) || !r.get(o.b) || !r.get(o.f)) return;
    Entry& e = g_entries[id];
    remember(e, o);
    e.todo.push_back(o);
    if (e.todo.size() > 64) e.todo.erase(e.todo.begin());
  }
}

void host_on_player_joined(net::Session& net) {
  for (const auto& [id, e] : g_entries) {
    if (e.app) net.send_game(encode(id, *e.app), true);
    for (const auto& o : e.ops) net.send_game(encode(id, o), true);
    if (e.state != State{}) net.send_game(encode(id, e.state), true);
  }
}

void clear() {
  g_entries.clear();
  g_throttle.clear();
  g_pending.clear();
  g_hosting = false;
}

} // namespace ivc::vehicle_props
