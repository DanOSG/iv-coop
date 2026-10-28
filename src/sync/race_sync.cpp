#include "sync/race_sync.h"

#include <map>
#include <vector>

#include "core/log.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "net/bytes.h"
#include "story/story_sync.h"

namespace ivc::race_sync {
namespace {
namespace n = natives;
using native_hooks::Call;

enum Op : std::uint8_t { OpCreate = 0, OpDelete, OpStartTrack, OpPoint, OpRender, OpLoop, OpClear };

constexpr std::uint32_t kCreateCheckpoint = 0x41F27499, kDeleteCheckpoint = 0x1293731D,
                        kStartTrack = 0x422C1818, kAddTrackPoint = 0x5BE115FD, kRenderTrack = 0x5062055B,
                        kLoopTrack = 0x77FD5097;
constexpr std::size_t kMaxCheckpoints = 64, kMaxPoints = 512;

struct Checkpoint {
  std::int32_t type;
  float x, y, z, a5, a6;
};

// Host: what's up now (for late joiners) and what to send.
bool g_hosting = false;
std::map<std::uint32_t, Checkpoint> g_checkpoints;
std::int32_t g_track_colour = 0;
bool g_track = false, g_render = false, g_loop = false;
std::vector<n::Vector3> g_points;
std::vector<std::vector<std::uint8_t>> g_out;

// Joiner: host id -> our checkpoint.
std::map<std::uint32_t, std::uint32_t> g_local;
bool g_local_render = false;

net::Writer msg(Op op) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(static_cast<std::uint8_t>(op));
  return w;
}

void queue(net::Writer&& w) {
  if (g_out.size() < 1024) g_out.push_back(std::move(w.data()));
}

bool capture(const Call& c) { return g_hosting && story::replicating(c.script); }

void put_checkpoint(net::Writer& w, std::uint32_t id, const Checkpoint& cp) {
  w.put(id).put(cp.type).put(cp.x).put(cp.y).put(cp.z).put(cp.a5).put(cp.a6);
}

} // namespace

void register_hooks() {
  using native_hooks::on;
  on(kCreateCheckpoint, "CREATE_CHECKPOINT", [](const Call& c) {
    if (!capture(c) || g_checkpoints.size() >= kMaxCheckpoints) return;
    const auto id = c.result<std::uint32_t>();
    const Checkpoint cp{c.arg<std::int32_t>(0), c.arg<float>(1), c.arg<float>(2), c.arg<float>(3), c.arg<float>(4),
                        c.arg<float>(5)};
    g_checkpoints[id] = cp;
    auto w = msg(OpCreate);
    put_checkpoint(w, id, cp);
    queue(std::move(w));
  });
  on(kDeleteCheckpoint, "DELETE_CHECKPOINT", [](const Call& c) {
    if (!capture(c)) return;
    const auto id = c.arg<std::uint32_t>(0);
    if (!g_checkpoints.erase(id)) return;
    auto w = msg(OpDelete);
    w.put(id);
    queue(std::move(w));
  });
  on(kStartTrack, "START_GPS_RACE_TRACK", [](const Call& c) {
    if (!capture(c)) return;
    g_track = true, g_track_colour = c.arg<std::int32_t>(0);
    g_points.clear();
    auto w = msg(OpStartTrack);
    w.put(g_track_colour);
    queue(std::move(w));
  });
  on(kAddTrackPoint, "ADD_POINT_TO_GPS_RACE_TRACK", [](const Call& c) {
    if (!capture(c) || g_points.size() >= kMaxPoints) return;
    const auto* p = c.arg<const n::Vector3*>(0);
    if (!p) return;
    g_points.push_back(*p);
    auto w = msg(OpPoint);
    w.put(p->x).put(p->y).put(p->z);
    queue(std::move(w));
  });
  on(kRenderTrack, "RENDER_RACE_TRACK", [](const Call& c) {
    if (!capture(c)) return;
    g_render = c.arg<std::uint32_t>(0) != 0;
    if (!g_render) g_track = false, g_points.clear();
    auto w = msg(OpRender);
    w.put(static_cast<std::uint8_t>(g_render));
    queue(std::move(w));
  });
  on(kLoopTrack, "LOOP_RACE_TRACK", [](const Call& c) {
    if (!capture(c)) return;
    g_loop = c.arg<std::uint32_t>(0) != 0;
    auto w = msg(OpLoop);
    w.put(static_cast<std::uint8_t>(g_loop));
    queue(std::move(w));
  });
}

void tick(bool hosting, net::Session& net) {
  if (g_hosting && !hosting) g_checkpoints.clear(), g_points.clear(), g_track = g_render = g_loop = false;
  g_hosting = hosting;
  for (auto& d : g_out) net.send_game(std::move(d), true);
  g_out.clear();
}

void host_on_player_joined(net::Session& net) {
  if (g_checkpoints.empty() && !g_track) return;
  std::vector<std::vector<std::uint8_t>> all;
  all.push_back(std::move(msg(OpClear).data()));
  for (const auto& [id, cp] : g_checkpoints) {
    auto w = msg(OpCreate);
    put_checkpoint(w, id, cp);
    all.push_back(std::move(w.data()));
  }
  if (g_track) {
    auto w = msg(OpStartTrack);
    w.put(g_track_colour);
    all.push_back(std::move(w.data()));
    for (const auto& p : g_points) {
      auto pw = msg(OpPoint);
      pw.put(p.x).put(p.y).put(p.z);
      all.push_back(std::move(pw.data()));
    }
    auto lw = msg(OpLoop);
    lw.put(static_cast<std::uint8_t>(g_loop));
    all.push_back(std::move(lw.data()));
    auto rw = msg(OpRender);
    rw.put(static_cast<std::uint8_t>(g_render));
    all.push_back(std::move(rw.data()));
  }
  for (auto& d : all) net.send_game(std::move(d), true);
}

void on_message(const net::GameMessage& m) {
  if (m.from != net::kHostId) return;
  net::Reader r(m.data);
  std::uint8_t type, sender, op;
  if (!r.get(type) || !r.get(sender) || !r.get(op)) return;
  switch (op) {
    case OpCreate: {
      std::uint32_t id;
      Checkpoint cp{};
      if (!r.get(id) || !r.get(cp.type) || !r.get(cp.x) || !r.get(cp.y) || !r.get(cp.z) || !r.get(cp.a5) ||
          !r.get(cp.a6))
        return;
      if (const auto it = g_local.find(id); it != g_local.end()) n::invoke(kDeleteCheckpoint, it->second);
      if (g_local.size() >= kMaxCheckpoints) return;
      g_local[id] = n::invoke<std::uint32_t>(kCreateCheckpoint, cp.type, cp.x, cp.y, cp.z, cp.a5, cp.a6);
      break;
    }
    case OpDelete: {
      std::uint32_t id;
      if (!r.get(id)) return;
      if (const auto it = g_local.find(id); it != g_local.end()) {
        n::invoke(kDeleteCheckpoint, it->second);
        g_local.erase(it);
      }
      break;
    }
    case OpStartTrack: {
      std::int32_t colour;
      if (r.get(colour)) n::invoke(kStartTrack, colour);
      break;
    }
    case OpPoint: {
      n::Vector3 p{};
      if (r.get(p.x) && r.get(p.y) && r.get(p.z)) n::invoke(kAddTrackPoint, &p);
      break;
    }
    case OpRender: {
      std::uint8_t on;
      if (!r.get(on)) return;
      g_local_render = on != 0;
      n::invoke(kRenderTrack, g_local_render);
      break;
    }
    case OpLoop: {
      std::uint8_t on;
      if (r.get(on)) n::invoke(kLoopTrack, on != 0);
      break;
    }
    case OpClear:
      clear();
      break;
    default:
      break;
  }
}

std::size_t local_count() { return g_local.size(); }

void clear() {
  for (const auto& [id, local] : g_local) n::invoke(kDeleteCheckpoint, local);
  g_local.clear();
  if (g_local_render) n::invoke(kRenderTrack, false), g_local_render = false;
}

} // namespace ivc::race_sync
