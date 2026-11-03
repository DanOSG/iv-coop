#include "sync/activities/activities.h"

#include <cmath>

// Each adapter: namespace ivc::activities::<name> { void register_hooks(); void host_tick(const Frame&); }
// declared here so the adapters stay self-contained files. ACTIVITY(name) lists them once.
#define IVC_ACTIVITIES(ACTIVITY) ACTIVITY(pool) ACTIVITY(darts) /* adapters are added here as they land */

namespace ivc::activities {

#define IVC_DECLARE(name)                                                                                            \
  namespace name {                                                                                                   \
  void register_hooks();                                                                                             \
  void host_tick(const Frame& f);                                                                                    \
  }
IVC_ACTIVITIES(IVC_DECLARE)
#undef IVC_DECLARE

void register_hooks() {
#define IVC_REGISTER(name) name::register_hooks();
  IVC_ACTIVITIES(IVC_REGISTER)
#undef IVC_REGISTER
}

void tick(const Frame& f) {
  if (!f.hosting) return;
#define IVC_TICK(name) name::host_tick(f);
  IVC_ACTIVITIES(IVC_TICK)
#undef IVC_TICK
}

natives::Ped host_ped() {
  const natives::Ped p = natives::GetPlayerChar(natives::GetPlayerId());
  return p && natives::DoesCharExist(p) ? p : 0;
}

std::vector<const Player*> partners_near(const Frame& f, float range) {
  std::vector<const Player*> out;
  const natives::Ped me = host_ped();
  if (!me) return out;
  const natives::Vector3 a = natives::GetCharCoordinates(me);
  for (const Player& p : f.players) {
    if (p.id == f.my_id || !p.ped || !natives::DoesCharExist(p.ped)) continue;
    const natives::Vector3 b = natives::GetCharCoordinates(p.ped);
    if (std::hypot(a.x - b.x, a.y - b.y) <= range && std::fabs(a.z - b.z) < 4.0f) out.push_back(&p);
  }
  return out;
}

} // namespace ivc::activities
