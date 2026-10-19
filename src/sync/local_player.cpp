#include "sync/local_player.h"

#include "game/natives.h"

namespace ivc::sync {
namespace n = natives;

std::optional<PlayerState> capture_local(std::uint32_t now_ms) {
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me || !n::DoesCharExist(me)) return std::nullopt;

  PlayerState s;
  const n::Vector3 pos = n::GetCharCoordinates(me);
  const n::Vector3 vel = n::GetCharVelocity(me);
  s.time_ms = now_ms;
  s.x = pos.x, s.y = pos.y, s.z = pos.z;
  s.vx = vel.x, s.vy = vel.y, s.vz = vel.z;
  s.heading = n::GetCharHeading(me);
  s.model = n::GetCharModel(me);
  s.in_vehicle = n::IsCharInAnyCar(me);
  return s;
}

} // namespace ivc::sync
