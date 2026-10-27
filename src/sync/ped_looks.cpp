#include "sync/ped_looks.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <variant>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "net/bytes.h"

namespace ivc::ped_looks {
namespace {
namespace n = natives;
using native_hooks::Call;

constexpr int kComponents = 9; // 0..8, as the game's own scripts read them
constexpr int kProps = 2;      // 0 hats/helmets, 1 glasses
constexpr std::int16_t kNoProp = -1;
constexpr DWORD kScanMs = 500;
constexpr int kMaxPendingFrames = 300;
constexpr DWORD kAnimRetryMs = 5000;

constexpr std::uint32_t kHashPropTexture = 0x57390041; // SET_CHAR_PROP_INDEX_TEXTURE (a2, a3, a4 raw)
constexpr std::uint32_t kHashClearAllProps = 0x232A52FA;
constexpr std::uint32_t kHashRequestAnims = 0x65F874DE;

enum class Sub : std::uint8_t { Looks = 0, State = 1, Anim = 2 };
enum class Field : std::uint8_t { ObjVisible = 0, ObjCollision = 1, ObjDynamic = 2, PedVisible = 3 };
constexpr int kFields = 4;

struct PropTex {
  std::int32_t a2, a3, a4;
  bool operator==(const PropTex&) const = default;
};
struct Looks {
  std::uint16_t drawable[kComponents]{}, texture[kComponents]{};
  std::int16_t prop[kProps] = {kNoProp, kNoProp};
  std::vector<PropTex> tex;
  bool operator==(const Looks&) const = default;
};
struct Anim {
  std::string name, dict;
  float speed = 0;
  std::uint8_t flag0 = 0, flag1 = 0;
};
struct Record {
  std::optional<Looks> looks;
  std::int8_t state[kFields] = {-1, -1, -1, -1}; // -1: never set
  std::optional<Anim> anim;
};

// ---------------------------------------------------------------- host

bool g_hosting = false;
bool g_rescan = false;
DWORD g_last_scan = 0;
std::map<std::uint32_t, Record> g_host;     // by networked id: what we last sent (and resend to joiners)
std::map<n::Handle, std::vector<PropTex>> g_tex; // SET_CHAR_PROP_INDEX_TEXTURE calls per ped, cleared with the props


struct Op {
  n::Handle handle = 0;
  bool is_anim = false;
  Field field = Field::ObjVisible;
  std::uint8_t value = 0;
  Anim anim;
  int age = 0;
};
std::deque<Op> g_ops; // script calls waiting for the entity to be networked

// ---------------------------------------------------------------- wire

void put_str(net::Writer& w, const std::string& s) {
  const auto len = static_cast<std::uint8_t>(std::min<std::size_t>(s.size(), 63));
  w.put(len);
  for (std::uint8_t i = 0; i < len; ++i) w.put(s[i]);
}
bool get_str(net::Reader& r, std::string& s) {
  std::uint8_t len;
  if (!r.get(len) || len > 63) return false;
  s.resize(len);
  for (auto& ch : s)
    if (!r.get(ch)) return false;
  return true;
}

std::vector<std::uint8_t> encode_looks(std::uint32_t id, const Looks& l) {
  net::Writer w;
  w.put(kMsg).put(static_cast<std::uint8_t>(Sub::Looks)).put(id);
  for (int i = 0; i < kComponents; ++i) w.put(l.drawable[i]).put(l.texture[i]);
  for (int i = 0; i < kProps; ++i) w.put(l.prop[i]);
  const auto nt = static_cast<std::uint8_t>(std::min<std::size_t>(l.tex.size(), 8));
  w.put(nt);
  for (std::uint8_t i = 0; i < nt; ++i) w.put(l.tex[i].a2).put(l.tex[i].a3).put(l.tex[i].a4);
  return std::move(w.data());
}
std::vector<std::uint8_t> encode_state(std::uint32_t id, Field f, std::uint8_t v) {
  net::Writer w;
  w.put(kMsg).put(static_cast<std::uint8_t>(Sub::State)).put(id).put(static_cast<std::uint8_t>(f)).put(v);
  return std::move(w.data());
}
std::vector<std::uint8_t> encode_anim(std::uint32_t id, const Anim& a) {
  net::Writer w;
  w.put(kMsg).put(static_cast<std::uint8_t>(Sub::Anim)).put(id);
  put_str(w, a.name);
  put_str(w, a.dict);
  w.put(a.speed).put(a.flag0).put(a.flag1);
  return std::move(w.data());
}

// ---------------------------------------------------------------- host capture

Looks read_looks(n::Handle ped) {
  Looks l;
  for (int c = 0; c < kComponents; ++c) {
    l.drawable[c] = static_cast<std::uint16_t>(n::GetCharDrawableVariation(ped, c));
    l.texture[c] = static_cast<std::uint16_t>(n::GetCharTextureVariation(ped, c));
  }
  for (int t = 0; t < kProps; ++t) {
    std::uint32_t idx = 0xFFFFFFFF;
    n::invoke(0x3AC85DB1, ped, static_cast<std::uint32_t>(t), &idx); // GET_CHAR_PROP_INDEX
    l.prop[t] = static_cast<std::int32_t>(idx) < 0 || idx > 0x7FFF ? kNoProp : static_cast<std::int16_t>(idx);
  }
  if (const auto it = g_tex.find(ped); it != g_tex.end()) l.tex = it->second;
  return l;
}

void state_op(const Call& c, Field f) {
  if (!g_hosting) return;
  Op op;
  op.handle = c.arg<n::Handle>(0), op.field = f, op.value = c.arg<std::uint32_t>(1) != 0;
  if (op.handle) g_ops.push_back(std::move(op));
}

void looks_changed(const Call&) {
  if (g_hosting) g_rescan = true;
}

void scan(net::Session& net, sync::EntitySync& ents) {
  const auto status = ents.status();
  std::vector<std::uint32_t> alive;
  for (const auto& s : status) {
    alive.push_back(s.id);
    if (s.type != sync::EntityType::Ped) continue;
    const n::Handle h = ents.handle_for_id(s.id);
    if (!h || !n::DoesCharExist(h)) continue;
    Looks l = read_looks(h);
    Record& r = g_host[s.id];
    if (r.looks && *r.looks == l) continue;
    r.looks = l;
    net.send_game(encode_looks(s.id, l), true);
  }
  for (auto it = g_host.begin(); it != g_host.end();)
    it = std::find(alive.begin(), alive.end(), it->first) == alive.end() ? g_host.erase(it) : std::next(it);
}

// ---------------------------------------------------------------- client

struct Client {
  Record rec;
  n::Handle applied = 0;
  bool looks_dirty = false, anim_dirty = false;
  std::uint8_t state_dirty = 0; // bit per Field
  DWORD anim_since = 0;
  DWORD born = GetTickCount();
};
std::map<std::uint32_t, Client> g_client;
DWORD g_last_prune = 0;

void apply_looks(n::Handle h, const Looks& l) {
  for (int c = 0; c < kComponents; ++c) n::SetCharComponentVariation(h, c, l.drawable[c], l.texture[c]);
  for (int t = 0; t < kProps; ++t) {
    if (l.prop[t] == kNoProp) n::invoke(0x51546112, h, static_cast<std::uint32_t>(t)); // CLEAR_CHAR_PROP
    else n::invoke(0x5FE95249, h, static_cast<std::uint32_t>(t), static_cast<std::uint32_t>(l.prop[t])); // SET_CHAR_PROP_INDEX
  }
  for (const auto& p : l.tex) {
    const std::uint32_t args[] = {static_cast<std::uint32_t>(h), static_cast<std::uint32_t>(p.a2),
                                  static_cast<std::uint32_t>(p.a3), static_cast<std::uint32_t>(p.a4)};
    n::invoke_raw(kHashPropTexture, args, 4);
  }
}

bool is_live(sync::EntitySync& ents, std::uint32_t id) {
  for (const auto& s : ents.status())
    if (s.id == id) return s.live;
  return false;
}

void apply_state(sync::EntitySync& ents, std::uint32_t id, n::Handle h, Field f, bool v) {
  switch (f) {
    case Field::ObjVisible: n::invoke(0x372C7B2A, h, static_cast<std::uint32_t>(v)); break;
    case Field::ObjCollision: n::invoke(0x24A40229, h, static_cast<std::uint32_t>(v)); break;
    case Field::ObjDynamic:
      // Follower copies are frozen on purpose; only ever make dynamic one we simulate.
      if (!v || is_live(ents, id)) n::invoke(0x2C591CCD, h, static_cast<std::uint32_t>(v));
      break;
    case Field::PedVisible: n::SetCharVisible(h, v); break;
  }
}

} // namespace

void register_hooks() {
  using native_hooks::on;
  on(0x71A52973, "SET_CHAR_COMPONENT_VARIATION", looks_changed);
  on(0x4FB30DB6, "SET_CHAR_DEFAULT_COMPONENT_VARIATION", looks_changed);
  on(0x47D9437C, "SET_CHAR_RANDOM_COMPONENT_VARIATION", looks_changed);
  on(0x5FE95249, "SET_CHAR_PROP_INDEX", looks_changed);
  on(0x51546112, "CLEAR_CHAR_PROP", looks_changed);
  on(kHashPropTexture, "SET_CHAR_PROP_INDEX_TEXTURE", [](const Call& c) {
    if (!g_hosting) return;
    const PropTex t{c.arg<std::int32_t>(1), c.arg<std::int32_t>(2), c.arg<std::int32_t>(3)};
    auto& v = g_tex[c.arg<n::Handle>(0)];
    const auto it = std::find_if(v.begin(), v.end(), [&](const PropTex& p) { return p.a2 == t.a2; });
    if (it != v.end()) *it = t;
    else v.push_back(t);
    g_rescan = true;
  });
  on(kHashClearAllProps, "CLEAR_ALL_CHAR_PROPS", [](const Call& c) {
    if (!g_hosting) return;
    g_tex.erase(c.arg<n::Handle>(0));
    g_rescan = true;
  });
  on(0x372C7B2A, "SET_OBJECT_VISIBLE", [](const Call& c) { state_op(c, Field::ObjVisible); });
  on(0x24A40229, "SET_OBJECT_COLLISION", [](const Call& c) { state_op(c, Field::ObjCollision); });
  on(0x2C591CCD, "SET_OBJECT_DYNAMIC", [](const Call& c) { state_op(c, Field::ObjDynamic); });
  on(0x04CF0105, "SET_CHAR_VISIBLE", [](const Call& c) { state_op(c, Field::PedVisible); });
  on(0x5D3241E4, "PLAY_OBJECT_ANIM", [](const Call& c) {
    if (!g_hosting) return;
    Op op;
    op.handle = c.arg<n::Handle>(0), op.is_anim = true;
    op.anim.name = c.str(1), op.anim.dict = c.str(2), op.anim.speed = c.arg<float>(3);
    op.anim.flag0 = c.arg<std::uint32_t>(4) != 0, op.anim.flag1 = c.arg<std::uint32_t>(5) != 0;
    if (op.handle) g_ops.push_back(std::move(op));
  });
}

void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents) {
  g_hosting = hosting;
  if (!hosting) {
    g_ops.clear(), g_host.clear(), g_tex.clear();
    return;
  }
  for (std::size_t left = g_ops.size(); left > 0; --left) {
    Op op = std::move(g_ops.front());
    g_ops.pop_front();
    const auto id = ents.id_for_handle(op.handle);
    if (!id) {
      if (++op.age < kMaxPendingFrames) g_ops.push_back(std::move(op)); // maybe not networked yet
      continue;
    }
    Record& r = g_host[*id];
    if (op.is_anim) {
      r.anim = op.anim;
      net.send_game(encode_anim(*id, op.anim), true);
    } else {
      std::int8_t& last = r.state[static_cast<int>(op.field)];
      if (last == static_cast<std::int8_t>(op.value)) continue; // a script repeating it every frame
      last = static_cast<std::int8_t>(op.value);
      net.send_game(encode_state(*id, op.field, op.value), true);
    }
  }
  const DWORD now = GetTickCount();
  if (g_rescan || now - g_last_scan >= kScanMs) {
    g_rescan = false, g_last_scan = now;
    scan(net, ents);
  }
}

void host_on_player_joined(net::Session& net) {
  for (const auto& [id, r] : g_host) {
    if (r.looks) net.send_game(encode_looks(id, *r.looks), true);
    for (int f = 0; f < kFields; ++f)
      if (r.state[f] >= 0) net.send_game(encode_state(id, static_cast<Field>(f), r.state[f]), true);
    if (r.anim) net.send_game(encode_anim(id, *r.anim), true);
  }
}

void on_message(const net::GameMessage& m) {
  if (m.from != net::kHostId) return;
  net::Reader r(m.data);
  std::uint8_t type, sub;
  std::uint32_t id;
  if (!r.get(type) || !r.get(sub) || !r.get(id)) return;
  Client& c = g_client[id];
  switch (static_cast<Sub>(sub)) {
    case Sub::Looks: {
      Looks l;
      for (int i = 0; i < kComponents; ++i)
        if (!r.get(l.drawable[i]) || !r.get(l.texture[i])) return;
      for (int i = 0; i < kProps; ++i)
        if (!r.get(l.prop[i])) return;
      std::uint8_t nt;
      if (!r.get(nt) || nt > 8) return;
      for (std::uint8_t i = 0; i < nt; ++i) {
        PropTex t;
        if (!r.get(t.a2) || !r.get(t.a3) || !r.get(t.a4)) return;
        l.tex.push_back(t);
      }
      if (c.rec.looks && *c.rec.looks == l) return; // a late-join resend
      c.rec.looks = std::move(l), c.looks_dirty = true;
      break;
    }
    case Sub::State: {
      std::uint8_t f, v;
      if (!r.get(f) || !r.get(v) || f >= kFields) return;
      c.rec.state[f] = static_cast<std::int8_t>(v != 0);
      c.state_dirty |= 1u << f;
      break;
    }
    case Sub::Anim: {
      Anim a;
      if (!get_str(r, a.name) || !get_str(r, a.dict) || !r.get(a.speed) || !r.get(a.flag0) || !r.get(a.flag1)) return;
      c.rec.anim = std::move(a), c.anim_dirty = true, c.anim_since = 0;
      break;
    }
  }
}

void client_tick(sync::EntitySync& ents) {
  const DWORD now = GetTickCount();
  for (auto& [id, c] : g_client) {
    const n::Handle h = ents.handle_for_id(id);
    if (!h) continue;
    if (h != c.applied) { // new (or respawned) copy: everything again
      c.applied = h;
      c.looks_dirty = c.rec.looks.has_value();
      c.state_dirty = 0;
      for (int f = 0; f < kFields; ++f)
        if (c.rec.state[f] >= 0) c.state_dirty |= 1u << f;
      c.anim_dirty = c.rec.anim.has_value(), c.anim_since = 0;
    }
    if (c.looks_dirty) {
      c.looks_dirty = false;
      if (n::DoesCharExist(h)) {
        apply_looks(h, *c.rec.looks);
        log::info("Entity {}: applied ped looks", id);
      }
    }
    for (int f = 0; f < kFields; ++f)
      if (c.state_dirty & (1u << f)) apply_state(ents, id, h, static_cast<Field>(f), c.rec.state[f] != 0);
    c.state_dirty = 0;
    if (c.anim_dirty && n::DoesObjectExist(h)) {
      if (!c.anim_since) c.anim_since = now ? now : 1;
      const Anim& a = *c.rec.anim;
      n::invoke(kHashRequestAnims, a.dict.c_str());
      const bool ok = n::invoke<bool>(0x5D3241E4, h, a.name.c_str(), a.dict.c_str(), a.speed,
                                      static_cast<std::uint32_t>(a.flag0), static_cast<std::uint32_t>(a.flag1));
      if (ok || now - c.anim_since > kAnimRetryMs) {
        c.anim_dirty = false;
        log::info("Entity {}: object anim {}/{} {}", id, a.dict, a.name, ok ? "playing" : "gave up");
      }
    }
  }
  if (now - g_last_prune > 2000) {
    g_last_prune = now;
    const auto status = ents.status();
    for (auto it = g_client.begin(); it != g_client.end();) {
      const bool known = std::any_of(status.begin(), status.end(), [&](const auto& s) { return s.id == it->first; });
      it = !known && now - it->second.born > 10000 ? g_client.erase(it) : std::next(it);
    }
  }
}

void clear() {
  g_ops.clear(), g_host.clear(), g_tex.clear(), g_client.clear();
  g_rescan = false;
}

} // namespace ivc::ped_looks
