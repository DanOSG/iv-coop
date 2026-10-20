#pragma once
// Vehicle replication (M3). Ownership rule: a vehicle's simulation is owned by the player
// driving it; with no player driving (e.g. a story character drives), by the lowest-id
// player inside. The owner sends VehicleState; everyone else shows a stand-in car driven
// kinematically from the interpolated state.
//
// Vehicle network ids are assigned by the owner: (owner player id << 24) | counter.
// A remote player's PlayerState names the vehicle id and seat they occupy, which may be
// one of *our* cars (e.g. riding as our passenger) or a remote stand-in.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "game/natives.h"
#include "net/session.h"
#include "sync/player_state.h"
#include "sync/vehicle_state.h"

namespace ivc::sync {

struct RemoteVehicleStatus {
  std::uint32_t vehicle_id;
  std::uint8_t owner;
  bool spawned;
  float error_m;
  std::uint32_t snaps;
  float gain; // physics-follow: learned velocity change per requested m/s (1 = impulse)
};

class VehicleSync {
 public:
  std::uint32_t interp_delay_ms = 100;
  std::uint32_t timeout_ms = 3000; // drop a remote car this long after its last update
  // Physics-follow: stand-ins stay physical (react to traffic/walls) and are steered with
  // APPLY_FORCE_TO_CAR. Off = kinematic (placed exactly on the pose every frame).
  bool physics_follow = true;
  std::uint32_t force_type = 1;

  // Returns the player id whose puppet is `ped`, if any (supplied by the player sync).
  using PuppetLookup = std::function<std::optional<std::uint8_t>(natives::Ped)>;

  // Fills `s.vehicle_id/seat` for the local player and, if we own that vehicle, returns
  // its state to send. Game thread, script context.
  std::optional<VehicleState> capture_local(natives::Ped me, std::uint8_t my_id, PlayerState& s,
                                            std::uint32_t now_ms, const PuppetLookup& puppet_of);

  void push_remote(const net::VehicleUpdate& u);
  // Spawns/drives/removes stand-in cars. Call once per frame after push_remote.
  void update_remote(std::uint32_t now_ms, natives::Ped me);

  // Local handle for a vehicle id (ours or a remote stand-in), or 0 if not (yet) present.
  natives::Vehicle handle_for(std::uint32_t vehicle_id) const;

  void drop_owner(std::uint8_t player_id); // owner left the session
  void clear();                            // session ended

  std::vector<RemoteVehicleStatus> status() const;

 private:
  struct Remote {
    std::uint8_t owner = 0;
    VehicleInterpolator interp;
    natives::Vehicle handle = 0;
    std::uint32_t model = 0;
    std::int8_t colour[4] = {-1, -1, -1, -1};
    std::uint8_t siren = 0;
    float error_m = 0;
    std::uint32_t snaps = 0;
    // Physics-follow controller state.
    natives::Vector3 last_vel{};
    natives::Vector3 last_request{}; // velocity change we asked for last frame (world, m/s)
    bool have_last = false;
    float gain = 1.0f;
  };

  std::uint32_t local_id_for(natives::Vehicle v, std::uint8_t my_id);
  void drive(Remote& r, const VehicleState& t);
  void drive_kinematic(Remote& r, const VehicleState& t);
  void drive_physics(Remote& r, const VehicleState& t);
  void snap(Remote& r, const VehicleState& t);
  void despawn(Remote& r, natives::Ped me);

  std::map<natives::Vehicle, std::uint32_t> local_ids_; // our cars -> network id
  std::uint8_t local_ids_owner_ = 0xFF;                 // player id the ids were minted under
  std::uint32_t next_counter_ = 1;
  std::map<std::uint32_t, Remote> remotes_;
  std::uint16_t seq_ = 0;
};

} // namespace ivc::sync
