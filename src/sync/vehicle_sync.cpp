#include "sync/vehicle_sync.h"

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

void VehicleSync::drive(Remote& r, const VehicleState& t) {
  const n::Vector3 cur = n::GetCarCoordinates(r.handle);
  r.error_m = std::hypot(t.x - cur.x, t.y - cur.y, t.z - cur.z);
  if (r.error_m > kSnapDist) ++r.snaps;
  // Kinematic: place the car exactly on the interpolated pose every frame, and give it the
  // owner's forward speed so wheels turn and the physics step carries it smoothly to the
  // next frame's pose.
  n::SetCarCoordinatesNoOffset(r.handle, {t.x, t.y, t.z});
  n::SetVehicleQuaternion(r.handle, {t.qx, t.qy, t.qz, t.qw});
  n::SetCarForwardSpeed(r.handle, t.speed);

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
      if (r.handle) drive(r, *t);
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
  for (const auto& [id, r] : remotes_) out.push_back({id, r.owner, r.handle != 0, r.error_m, r.snaps});
  return out;
}

} // namespace ivc::sync
