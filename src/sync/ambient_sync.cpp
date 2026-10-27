#include "sync/ambient_sync.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "core/log.h"

namespace ivc::sync::ambient {
namespace n = natives;
namespace {

constexpr float kRegisterM = 60.0f;    // an entity must be this close to the host and to a partner
constexpr float kReleaseM = 90.0f;     // ...and is released once fewer than two players are within this
constexpr float kPartnerMaxM = 130.0f; // partners further apart than this share no zone
constexpr std::size_t kMaxPeds = 16, kMaxCars = 12;
constexpr std::uint32_t kScanMs = 700;
constexpr int kMaxNewPerScan = 3;
constexpr int kPedTries = 4, kCarTries = 3;

// Joiner thinning.
constexpr float kThinOnM = 100.0f, kThinOffM = 130.0f; // host this close: thin our own population
constexpr float kThinDensity = 0.3f;
constexpr int kMaxDeletesPerScan = 3;

constexpr std::uint32_t kGetRandomCharInAreaOffsetNoSave = 0x6ED17CF8, kGetRandomCarInSphereNoSave = 0x0A7E36E5,
                        kIsPedAMissionPed = 0x05801768, kIsCarAMissionCar = 0x7A422E14,
                        kIsThisPedAPlayer = 0x37C85316, kIsCarDead = 0x2AAB340A, kIsCharOnScreen = 0x59471B11,
                        kIsCarOnScreen = 0x59E3553F, kMarkCarNoLongerNeeded = 0x20C76FD1,
                        kMarkCharNoLongerNeeded = 0x0B774604, kSetPedDensity = 0x540F2DF7,
                        kSetCarDensity = 0x0AA73A12, kSetRandomCarDensity = 0x073505E0,
                        kSetParkedCarDensity = 0x010C7044;

struct Tracked {
  EntityType type;
  n::Handle handle;
};

std::map<std::uint32_t, Tracked> g_tracked; // host: ambient entities we registered
std::uint32_t g_last_scan = 0;
bool g_thinning = false;
std::uint32_t g_last_density = 0;

float dist(const n::Vector3& a, const n::Vector3& b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); }

n::Ped random_ped(const n::Vector3& c, float half) {
  n::Ped p = 0;
  n::invoke(kGetRandomCharInAreaOffsetNoSave, c.x, c.y, c.z, half, half, half, &p);
  return p;
}
n::Vehicle random_car(const n::Vector3& c, float radius) {
  n::Vehicle v = 0;
  n::invoke(kGetRandomCarInSphereNoSave, c.x, c.y, c.z, radius, 0u, false, &v);
  return v;
}

struct Ctx {
  EntitySync& ents;
  const std::vector<PlayerPos>& players;
  std::set<n::Handle> player_peds; // our copy of every player (us, extra player peds, puppets)
  std::set<n::Vehicle> player_cars;
};

Ctx make_ctx(EntitySync& ents, const std::vector<PlayerPos>& players) {
  Ctx c{ents, players, {}, {}};
  for (const PlayerPos& p : players) {
    const n::Ped ped = ents.player_ped ? ents.player_ped(p.id) : 0;
    if (!ped || !n::DoesCharExist(ped)) continue;
    c.player_peds.insert(ped);
    if (n::IsCharInAnyCar(ped)) c.player_cars.insert(n::StoreCarCharIsInNoSave(ped));
  }
  return c;
}

// An ordinary city ped that nothing else owns.
bool free_ped(const Ctx& c, n::Ped p) {
  if (!p || !n::DoesCharExist(p) || n::IsCharDead(p)) return false;
  if (c.player_peds.contains(p) || n::invoke<bool>(kIsThisPedAPlayer, p) || n::invoke<bool>(kIsPedAMissionPed, p)) return false;
  return !c.ents.id_for_handle(p);
}
bool free_car(const Ctx& c, n::Vehicle v) {
  if (!v || !n::DoesVehicleExist(v) || n::invoke<bool>(kIsCarDead, v)) return false;
  if (c.player_cars.contains(v) || n::invoke<bool>(kIsCarAMissionCar, v)) return false;
  return !c.ents.id_for_handle(v);
}

void release(Ctx& c, net::Session& net, std::uint32_t id, const Tracked& t, const char* why) {
  n::Handle h = c.ents.host_release(id, net);
  if (!h) return;
  if (t.type == EntityType::Ped)
    n::invoke(kMarkCharNoLongerNeeded, &h); // the engine may clean it up again
  else
    n::invoke(kMarkCarNoLongerNeeded, &h);
  log::info("Ambient {} released ({})", id, why);
}

void release_all(Ctx& c, net::Session& net) {
  for (const auto& [id, t] : g_tracked) release(c, net, id, t, "ambient sync off");
  g_tracked.clear();
}

void set_density(float f) {
  n::invoke(kSetPedDensity, f);
  n::invoke(kSetCarDensity, f);
  n::invoke(kSetRandomCarDensity, f);
  n::invoke(kSetParkedCarDensity, f);
}

// ---------------------------------------------------------------- host

std::size_t count_of(EntityType t) {
  std::size_t k = 0;
  for (const auto& [id, tr] : g_tracked) k += tr.type == t;
  return k;
}

void track(Ctx& c, net::Session& net, n::Handle h, EntityType type, std::uint32_t model) {
  if (const auto id = c.ents.host_register(h, type, model, net, /*pinned=*/false, /*ambient=*/true))
    g_tracked[*id] = {type, h};
}

void host_scan(Ctx& c, std::uint8_t my_id, net::Session& net) {
  const PlayerPos* host = nullptr;
  for (const PlayerPos& p : c.players)
    if (p.id == my_id) host = &p;
  if (!host) return;

  // Release what is no longer near two players (or vanished from the world).
  std::map<std::uint32_t, EntityStatus> st;
  for (const EntityStatus& s : c.ents.status()) st[s.id] = s;
  for (auto it = g_tracked.begin(); it != g_tracked.end();) {
    const auto s = st.find(it->first);
    const char* why = nullptr;
    if (s == st.end() || !c.ents.handle_for_id(it->first)) {
      why = "gone";
    } else {
      int near_players = 0;
      for (const PlayerPos& p : c.players) near_players += dist({s->second.x, s->second.y, s->second.z}, p.pos) < kReleaseM;
      if (near_players < 2) why = "no longer near two players";
    }
    if (why) {
      if (s != st.end()) release(c, net, it->first, it->second, why);
      else log::info("Ambient {} dropped (entity gone)", it->first);
      it = g_tracked.erase(it);
    } else {
      ++it;
    }
  }

  // Register new ones in the overlap of the host and each nearby partner.
  int added = 0;
  for (const PlayerPos& partner : c.players) {
    if (partner.id == my_id || added >= kMaxNewPerScan) continue;
    if (dist(host->pos, partner.pos) > kPartnerMaxM) continue;
    const n::Vector3 mid{(host->pos.x + partner.pos.x) / 2, (host->pos.y + partner.pos.y) / 2,
                         (host->pos.z + partner.pos.z) / 2};
    auto in_zone = [&](const n::Vector3& p) {
      if (dist(p, host->pos) > kRegisterM) return false;
      for (const PlayerPos& q : c.players)
        if (q.id != my_id && dist(p, q.pos) <= kRegisterM) return true;
      return false;
    };
    for (int i = 0; i < kCarTries && added < kMaxNewPerScan; ++i) {
      const n::Vehicle v = random_car(mid, kRegisterM + 20.0f);
      if (!free_car(c, v) || !in_zone(n::GetCarCoordinates(v))) continue;
      // Only moving traffic: a car with an ordinary driver (parked cars are static and numerous).
      const n::Ped driver = n::GetDriverOfCar(v);
      if (!free_ped(c, driver) || count_of(EntityType::Ped) + 1 > kMaxPeds || count_of(EntityType::Vehicle) >= kMaxCars) continue;
      const std::uint32_t car_model = n::GetCarModel(v), ped_model = n::GetCharModel(driver);
      if (!car_model || !ped_model) continue;
      track(c, net, v, EntityType::Vehicle, car_model); // car first, so the driver's copy can be seated
      track(c, net, driver, EntityType::Ped, ped_model);
      ++added;
    }
    for (int i = 0; i < kPedTries && added < kMaxNewPerScan; ++i) {
      const n::Ped p = random_ped(mid, kRegisterM + 20.0f);
      if (!free_ped(c, p) || n::IsCharInAnyCar(p) || count_of(EntityType::Ped) >= kMaxPeds) continue;
      if (!in_zone(n::GetCharCoordinates(p))) continue;
      const std::uint32_t model = n::GetCharModel(p);
      if (!model) continue;
      track(c, net, p, EntityType::Ped, model);
      ++added;
    }
  }
  if (added)
    log::info("Ambient: shared {} more ({} peds, {} cars)", added, count_of(EntityType::Ped), count_of(EntityType::Vehicle));
}

// ---------------------------------------------------------------- joiner

void joiner_thin(Ctx& c, const n::Vector3& host_pos, const n::Vector3& me) {
  auto in_zone = [&](const n::Vector3& p) { return dist(p, host_pos) <= kRegisterM && dist(p, me) <= kRegisterM; };
  if (n::StoreWantedLevel(n::GetPlayerId()) > 0) return; // cops stay
  const n::Vector3 mid{(host_pos.x + me.x) / 2, (host_pos.y + me.y) / 2, (host_pos.z + me.z) / 2};
  int deleted = 0;
  for (int i = 0; i < kCarTries && deleted < kMaxDeletesPerScan; ++i) {
    n::Vehicle v = random_car(mid, kRegisterM + 20.0f);
    if (!free_car(c, v) || !in_zone(n::GetCarCoordinates(v)) || n::invoke<bool>(kIsCarOnScreen, v)) continue;
    n::Ped driver = n::GetDriverOfCar(v);
    if (driver && n::DoesCharExist(driver)) {
      if (c.player_peds.contains(driver) || n::invoke<bool>(kIsPedAMissionPed, driver) || c.ents.id_for_handle(driver)) continue;
      n::DeleteChar(driver);
    }
    n::DeleteCar(v);
    ++deleted;
  }
  for (int i = 0; i < kPedTries && deleted < kMaxDeletesPerScan; ++i) {
    n::Ped p = random_ped(mid, kRegisterM + 20.0f);
    if (!free_ped(c, p) || n::IsCharInAnyCar(p) || !in_zone(n::GetCharCoordinates(p)) || n::invoke<bool>(kIsCharOnScreen, p)) continue;
    n::DeleteChar(p);
    ++deleted;
  }
}

} // namespace

std::size_t shared_count() { return g_tracked.size(); }

void tick(std::uint32_t now_ms, bool in_session, bool hosting, bool enabled, std::uint8_t my_id,
          const std::vector<PlayerPos>& players, EntitySync& ents, net::Session& net) {
  if (!in_session) { // EntitySync::clear() has dealt with the entities
    g_tracked.clear();
    if (g_thinning) set_density(1.0f), g_thinning = false;
    return;
  }
  Ctx c = make_ctx(ents, players);
  if (hosting) {
    if (g_thinning) set_density(1.0f), g_thinning = false;
    if (!enabled) {
      if (!g_tracked.empty()) release_all(c, net);
      return;
    }
    if (now_ms - g_last_scan >= kScanMs) {
      g_last_scan = now_ms;
      host_scan(c, my_id, net);
    }
    return;
  }

  // Joiner.
  const PlayerPos *host = nullptr, *me = nullptr;
  for (const PlayerPos& p : players) {
    if (p.id == net::kHostId) host = &p;
    if (p.id == my_id) me = &p;
  }
  const float d = host && me ? dist(host->pos, me->pos) : 1e9f;
  const bool want = enabled && d < (g_thinning ? kThinOffM : kThinOnM);
  if (want != g_thinning) {
    g_thinning = want;
    g_last_density = 0;
    if (!want) set_density(1.0f);
    log::info("Ambient: {} local population near the host", want ? "thinning" : "restored");
  }
  if (!want) return;
  if (now_ms - g_last_density >= 3000) { // scripts may reset the multipliers; keep ours
    g_last_density = now_ms;
    set_density(kThinDensity);
  }
  if (now_ms - g_last_scan >= kScanMs) {
    g_last_scan = now_ms;
    joiner_thin(c, host->pos, me->pos);
  }
}

} // namespace ivc::sync::ambient
