#include "sync/vehicle_sync.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "core/log.h"

namespace ivc::sync {
namespace n = natives;
namespace {

constexpr float kSnapDist = 10.0f; // m of error before a stand-in car is teleported instead of steered

std::uint8_t owner_of(std::uint32_t vehicle_id) { return static_cast<std::uint8_t>(vehicle_id >> 24); }

// Seat the local player occupies in `v`: -1 driver, 0.. passenger, or nullopt if not found.
std::optional<std::int8_t> seat_of(n::Vehicle v, n::Ped ped) {
  if (n::GetDriverOfCar(v) == ped) return static_cast<std::int8_t>(n::kDriverSeat);
  const std::uint32_t max = n::GetMaximumNumberOfPassengers(v);
  for (std::uint32_t i = 0; i < max && i < 8; ++i)
    if (n::GetCharInCarPassengerSeat(v, i) == ped) return static_cast<std::int8_t>(i);
  return std::nullopt;
}

} // namespace

std::uint32_t VehicleSync::local_id_for(n::Vehicle v, std::uint8_t my_id) {
  if (local_ids_owner_ != my_id) { // new session / new player id: ids minted earlier are void
    local_ids_.clear();
    local_ids_owner_ = my_id;
  }
  if (auto it = local_ids_.find(v); it != local_ids_.end()) return it->second;
  const std::uint32_t id = (static_cast<std::uint32_t>(my_id) << 24) | (next_counter_++ & 0xFFFFFF);
  local_ids_.emplace(v, id);
  return id;
}

std::optional<VehicleState> VehicleSync::capture_local(n::Ped me, std::uint8_t my_id, PlayerState& s,
                                                       std::uint32_t now_ms, const PuppetLookup& puppet_of) {
  s.vehicle_id = kNoVehicle;
  s.seat = n::kDriverSeat;
  if (!s.in_vehicle) return std::nullopt;

  const n::Vehicle v = n::StoreCarCharIsInNoSave(me);
  if (!v || !n::DoesVehicleExist(v)) return std::nullopt;
  const auto seat = seat_of(v, me);
  if (!seat) return std::nullopt;
  s.seat = *seat;

  // Riding in someone else's stand-in car: name it by their id; it's theirs to simulate.
  for (const auto& [id, r] : remotes_) {
    if (r.handle == v) {
      s.vehicle_id = id;
      return std::nullopt;
    }
  }

  // One of our cars (or a world car we're in): we own it if we drive, or if no player
  // drives and no lower-id player rides along.
  const n::Ped driver = n::GetDriverOfCar(v);
  bool own = *seat == n::kDriverSeat;
  if (!own) {
    own = !(driver && puppet_of(driver)); // a remote player is driving: theirs
    const std::uint32_t max = n::GetMaximumNumberOfPassengers(v);
    for (std::uint32_t i = 0; own && i < max && i < 8; ++i) {
      const n::Ped p = n::GetCharInCarPassengerSeat(v, i);
      if (auto pid = p ? puppet_of(p) : std::nullopt; pid && *pid < my_id) own = false;
    }
  }
  s.vehicle_id = local_id_for(v, my_id);
  if (!own) return std::nullopt;

  VehicleState out;
  out.time_ms = now_ms;
  out.seq = ++seq_;
  out.vehicle_id = s.vehicle_id;
  out.model = n::GetCarModel(v);
  const n::Vector3 p = n::GetCarCoordinates(v);
  out.x = p.x, out.y = p.y, out.z = p.z;
  const n::Vector4 q = n::GetVehicleQuaternion(v);
  out.qx = q.x, out.qy = q.y, out.qz = q.z, out.qw = q.w;
  const n::Vector3 vel = n::GetCarSpeedVector(v);
  out.vx = vel.x, out.vy = vel.y, out.vz = vel.z;
  // Signed forward speed: the game reports magnitude; the sign comes from whether we're
  // moving along or against the car's heading.
  const float h = n::GetCarHeading(v) * std::numbers::pi_v<float> / 180.0f;
  const float along = vel.x * -std::sin(h) + vel.y * std::cos(h);
  out.speed = along < 0 ? -n::GetCarSpeed(v) : n::GetCarSpeed(v);
  std::int32_t c1 = 0, c2 = 0, c3 = 0, c4 = 0;
  n::GetCarColours(v, c1, c2);
  n::GetExtraCarColours(v, c3, c4);
  out.colour[0] = static_cast<std::int8_t>(c1), out.colour[1] = static_cast<std::int8_t>(c2);
  out.colour[2] = static_cast<std::int8_t>(c3), out.colour[3] = static_cast<std::int8_t>(c4);
  out.health = static_cast<std::uint16_t>(std::min<std::uint32_t>(n::GetCarHealth(v), 0xFFFF));
  out.engine_health = n::GetEngineHealth(v);
  out.siren = n::IsCarSirenOn(v);
  return out;
}

void VehicleSync::push_remote(const net::VehicleUpdate& u) {
  if (owner_of(u.state.vehicle_id) == local_ids_owner_ && local_ids_owner_ != 0xFF) {
    // State for one of *our* ids: only happens if ownership moved to another player while
    // our id stays in use. Not handled yet (M4 migration); ignore rather than fight.
    return;
  }
  auto [it, inserted] = remotes_.try_emplace(u.state.vehicle_id);
  Remote& r = it->second;
  if (inserted) {
    r.owner = u.id;
    r.interp.delay_ms = interp_delay_ms;
  }
  r.interp.push(u.state, u.arrival_ms);
}

void VehicleSync::snap(Remote& r, const VehicleState& t) {
  n::SetCarCoordinatesNoOffset(r.handle, {t.x, t.y, t.z});
  n::SetVehicleQuaternion(r.handle, {t.qx, t.qy, t.qz, t.qw});
  n::SetCarForwardSpeed(r.handle, t.speed);
  r.have_last = false; // the teleport invalidates the velocity measurement
  r.blocked_s = 0;
  ++r.snaps;
}

// Kinematic: place the car exactly on the interpolated pose every frame, and give it the
// owner's forward speed so wheels turn and the physics step carries it smoothly to the
// next frame's pose. Exact, but passes through anything local (walls, local traffic).
void VehicleSync::drive_kinematic(Remote& r, const VehicleState& t) {
  if (r.error_m > kSnapDist) ++r.snaps;
  n::SetCarCoordinatesNoOffset(r.handle, {t.x, t.y, t.z});
  n::SetVehicleQuaternion(r.handle, {t.qx, t.qy, t.qz, t.qw});
  n::SetCarForwardSpeed(r.handle, t.speed);
}

// Physics-follow: the stand-in stays a physical car; each frame we request the velocity
// change that brings it onto the owner's track (their velocity plus a pull toward their
// position) through APPLY_FORCE_TO_CAR with mass scaling. Which force type is an impulse
// isn't known statically, so the controller measures how much velocity change it actually
// got per m/s requested (horizontal only, gravity-free) and scales its requests by that.
void VehicleSync::drive_physics(Remote& r, const VehicleState& t, float dt) {
  constexpr float kPull = 2.0f;      // 1/s: position error -> corrective velocity
  constexpr float kMaxPull = 8.0f;   // m/s cap on the corrective part
  // Authority: a car-like 2 g. Enough to hold a fast circle, but weak enough that walls
  // and other cars win the contact, so the stand-in stops/deflects instead of being
  // bulldozed through (it then snaps once it's kSnapDist behind).
  constexpr float kMaxAccel = 20.0f; // m/s^2
  constexpr float kSnapDz = 4.0f;
  constexpr float kRotBlend = 0.15f; // fraction of the orientation error closed per frame

  const n::Vector3 cur = n::GetCarCoordinates(r.handle);
  if (r.error_m > kSnapDist || std::fabs(t.z - cur.z) > kSnapDz) {
    snap(r, t);
    return;
  }
  const n::Vector3 vel = n::GetCarSpeedVector(r.handle);

  // Learn the gain from last frame's request (horizontal, only when the request was big
  // enough to measure over noise from friction, collisions and the engine).
  if (r.have_last) {
    const float rx = r.last_request.x, ry = r.last_request.y;
    const float req2 = rx * rx + ry * ry;
    if (req2 > 0.01f) { // requests are now per-frame sized (<= 2 g * dt), so a lower floor
      const float got = ((vel.x - r.last_vel.x) * rx + (vel.y - r.last_vel.y) * ry) / req2;
      if (got > 0.001f && got < 3.0f) r.gain += (got - r.gain) * 0.1f;
    }
  }

  // Steering: turn the front wheels by the owner's yaw rate (bicycle model) so they point
  // into curves. (Tight turns still screech, as a real car would.)
  const float yaw = std::atan2(2.0f * (t.qw * t.qz + t.qx * t.qy), 1.0f - 2.0f * (t.qy * t.qy + t.qz * t.qz));
  if (r.have_yaw) {
    float dyaw = yaw - r.last_target_yaw;
    if (dyaw > std::numbers::pi_v<float>) dyaw -= 2 * std::numbers::pi_v<float>;
    if (dyaw < -std::numbers::pi_v<float>) dyaw += 2 * std::numbers::pi_v<float>;
    const float rate = dyaw / dt;
    constexpr float kWheelbase = 2.7f, kMaxSteer = 0.6f; // m, rad
    const float speed = std::max(std::fabs(t.speed), 2.0f);
    const float target_steer = std::clamp(std::atan(kWheelbase * rate / speed) / kMaxSteer, -1.0f, 1.0f);
    r.steer += (target_steer - r.steer) * 0.2f; // yaw rate from 30 Hz snapshots is jumpy
  }
  r.have_yaw = true;
  r.last_target_yaw = yaw;
  n::SetVehicleSteerBias(r.handle, r.steer);

  // Horizontal only: gravity and suspension keep the car on the road; height mismatches
  // beyond kSnapDz are fixed by snapping.
  const float ex = t.x - cur.x, ey = t.y - cur.y;
  const float pull = std::hypot(ex, ey) * kPull;
  const float pull_scale = pull > kMaxPull ? kMaxPull / pull : 1.0f;
  const float dvx = t.vx + ex * kPull * pull_scale - vel.x;
  const float dvy = t.vy + ey * kPull * pull_scale - vel.y;
  const float dv = std::hypot(dvx, dvy);
  const float max_dv = kMaxAccel * dt;
  const float dv_scale = dv > max_dv ? max_dv / dv : 1.0f;
  const n::Vector3 want{dvx * dv_scale, dvy * dv_scale, 0.0f};

  // Blocked: well off track and not moving toward it (pinned against a wall, or stuck
  // under traffic). Keep pushing and the solver lets us creep through the geometry; so
  // stop pushing, sit there like a stuck car would, and let the snap catch up.
  constexpr float kBlockedErr = 3.0f, kBlockedAfter = 0.75f;
  const float err = std::hypot(ex, ey);
  const float toward = err > 0.01f ? (vel.x * ex + vel.y * ey) / err : 0.0f;
  if (err > kBlockedErr && toward < 1.5f)
    r.blocked_s += dt;
  else
    r.blocked_s = std::max(0.0f, r.blocked_s - 2 * dt);
  if (r.blocked_s > kBlockedAfter) {
    r.have_last = false;
    return; // no force, no rotation: let the contact hold
  }

  const float g = std::max(r.gain, 0.005f);
  n::ApplyForceToCar(r.handle, force_type, {want.x / g, want.y / g, want.z / g});
  r.last_vel = vel;
  r.last_request = want;
  r.have_last = true;

  // Ease the orientation toward the owner's (short-arc nlerp of the current one).
  // (Tried and dropped: re-asserting forward speed each frame to "sync" wheel spin; it
  // overwrites vertical velocity and fights the suspension, so the car sinks.)
  const n::Vector4 q = n::GetVehicleQuaternion(r.handle);
  const float dot = q.x * t.qx + q.y * t.qy + q.z * t.qz + q.w * t.qw;
  const float s = dot < 0 ? -1.0f : 1.0f;
  n::Vector4 o{q.x + (s * t.qx - q.x) * kRotBlend, q.y + (s * t.qy - q.y) * kRotBlend,
               q.z + (s * t.qz - q.z) * kRotBlend, q.w + (s * t.qw - q.w) * kRotBlend};
  const float len = std::sqrt(o.x * o.x + o.y * o.y + o.z * o.z + o.w * o.w);
  if (len > 1e-6f) n::SetVehicleQuaternion(r.handle, {o.x / len, o.y / len, o.z / len, o.w / len});
}

void VehicleSync::drive(Remote& r, const VehicleState& t, std::uint32_t now_ms) {
  const n::Vector3 cur = n::GetCarCoordinates(r.handle);
  r.error_m = std::hypot(t.x - cur.x, t.y - cur.y);
  const float dt = r.last_drive_ms ? std::clamp((now_ms - r.last_drive_ms) / 1000.0f, 0.004f, 0.05f) : 1.0f / 60;
  r.last_drive_ms = now_ms;

  if (physics_follow)
    drive_physics(r, t, dt);
  else
    drive_kinematic(r, t);

  if (std::memcmp(r.colour, t.colour, 4) != 0) {
    n::ChangeCarColour(r.handle, t.colour[0], t.colour[1]);
    n::SetExtraCarColours(r.handle, t.colour[2], t.colour[3]);
    std::memcpy(r.colour, t.colour, 4);
  }
  if (r.siren != t.siren) {
    n::SwitchCarSiren(r.handle, t.siren != 0);
    r.siren = t.siren;
  }
  // Damage visuals follow the owner's numbers (smoke/fire); real damage sync comes later.
  if (std::abs(static_cast<int>(n::GetCarHealth(r.handle)) - t.health) > 25) n::SetCarHealth(r.handle, t.health);
  if (std::abs(n::GetEngineHealth(r.handle) - t.engine_health) > 25.0f) n::SetEngineHealth(r.handle, t.engine_health);
}

void VehicleSync::despawn(Remote& r, n::Ped me) {
  if (r.handle && n::DoesVehicleExist(r.handle)) {
    if (me && n::IsCharInCar(me, r.handle)) {
      n::WarpCharFromCarToCoord(me, n::GetCarCoordinates(r.handle)); // never delete a car we sit in
    }
    n::DeleteCar(r.handle);
  }
  r.handle = 0;
}

void VehicleSync::update_remote(std::uint32_t now_ms, n::Ped me) {
  for (auto it = remotes_.begin(); it != remotes_.end();) {
    Remote& r = it->second;
    if (now_ms - r.interp.last_arrival_ms() > timeout_ms) {
      log::info("Vehicle 0x{:08X} (owner {}) timed out; removing stand-in", it->first, r.owner);
      despawn(r, me);
      it = remotes_.erase(it);
      continue;
    }
    if (auto t = r.interp.sample(now_ms)) {
      if (r.handle && !n::DoesVehicleExist(r.handle)) r.handle = 0; // the game removed it
      if (!r.handle || r.model != t->model) {
        if (r.handle) despawn(r, me);
        n::RequestModel(t->model);
        if (n::HasModelLoaded(t->model)) {
          r.handle = n::CreateCar(t->model, {t->x, t->y, t->z});
          n::MarkModelAsNoLongerNeeded(t->model);
          if (r.handle) {
            r.model = t->model;
            std::memset(r.colour, -1, 4);
            n::SetCarCanBeDamaged(r.handle, false); // owner decides damage
            n::SetCarEngineOn(r.handle, true, true);
            log::info("Vehicle 0x{:08X} (owner {}): stand-in spawned, handle {} model 0x{:08X}", it->first, r.owner,
                      r.handle, t->model);
          }
        }
      }
      if (r.handle) drive(r, *t, now_ms);
    }
    ++it;
  }
}

n::Vehicle VehicleSync::handle_for(std::uint32_t vehicle_id) const {
  if (vehicle_id == kNoVehicle) return 0;
  if (auto it = remotes_.find(vehicle_id); it != remotes_.end()) return it->second.handle;
  for (const auto& [handle, id] : local_ids_)
    if (id == vehicle_id) return n::DoesVehicleExist(handle) ? handle : 0;
  return 0;
}

void VehicleSync::drop_owner(std::uint8_t player_id) {
  for (auto it = remotes_.begin(); it != remotes_.end();) {
    if (it->second.owner == player_id) {
      despawn(it->second, 0);
      it = remotes_.erase(it);
    } else {
      ++it;
    }
  }
}

void VehicleSync::clear() {
  for (auto& [id, r] : remotes_) despawn(r, 0);
  remotes_.clear();
  local_ids_.clear();
  local_ids_owner_ = 0xFF;
}

std::vector<RemoteVehicleStatus> VehicleSync::status() const {
  std::vector<RemoteVehicleStatus> out;
  for (const auto& [id, r] : remotes_)
    out.push_back({id, r.owner, r.handle != 0, r.error_m, r.snaps, r.gain, r.blocked_s > 0.75f, r.steer});
  return out;
}

} // namespace ivc::sync
