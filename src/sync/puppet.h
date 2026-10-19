#pragma once
// A script-owned ped that we position directly every frame: the M1/M2 stand-in for a remote
// player (frozen, no collision, invincible, AI events blocked). M3+ replaces this with a
// real player-slot ped driven by replicated pad input.

#include <cstdint>
#include <string>

#include "game/natives.h"

namespace ivc::sync {

class Puppet {
 public:
  explicit Puppet(std::string model_name = "M_Y_MULTIPLAYER") : model_name_(std::move(model_name)) {}

  // Game thread, script context. Requests the model and spawns when it's loaded; returns
  // true once the ped exists (respawning it if the game removed it).
  bool ensure(const natives::Vector3& at);
  void place(const natives::Vector3& at, float heading);
  void despawn();

  bool spawned() const { return ped_ != 0; }
  natives::Ped ped() const { return ped_; }
  // Short description of what ensure() is waiting on, for status displays.
  const char* phase() const { return phase_; }

 private:
  std::string model_name_;
  std::uint32_t model_ = 0;
  natives::Ped ped_ = 0;
  natives::Blip blip_ = 0;
  const char* phase_ = "idle";
};

} // namespace ivc::sync
