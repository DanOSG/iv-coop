#pragma once
// Downed and revive (ARCHITECTURE, "Deaths: downed and revive").
//
// During a mission (the host's GET_MISSION_FLAG), a player whose health runs out goes down
// instead of dying: health held at 110 (the player dies below 100), invincible, lying
// injured on the ground (InjuredPose). A partner revives them by holding E next to them for ~6 s while
// playing the medics' CPR clip; they come back on half health (150 of 100..200). A joiner
// left down bleeds out after 60 s and dies normally (hospital); the host never bleeds out.
// When every player is down at once, the host sends AllDown and everyone dies for real, so
// the mission fails as normal. Outside missions deaths are normal and per player.
// Limit: a single hit large enough to go from healthy to dead in one frame (explosions)
// still kills; catching it would need a health buffer the HUD would show.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/natives.h"
#include "net/session.h"

namespace ivc::downed {

enum class Msg : std::uint8_t { MissionFlag = 40, Revive = 41, AllDown = 42 };

// The "hurt on the ground" stance: a ragdoll fall, then (once it settles) the lying-injured
// loop scripts use for beaten peds (missbrian_3 / Injured_Idle). For our downed player and
// for downed partners' puppets. update() every frame; up(ped) to get back on their feet.
class InjuredPose {
 public:
  void update(natives::Ped ped, std::uint32_t now_ms);
  void up(natives::Ped ped);
  bool down() const { return state_ != State::Standing; }

 private:
  enum class State { Standing, Falling, Lying } state_ = State::Standing;
  std::uint32_t since_ = 0;
};
bool is_msg(std::span<const std::uint8_t> d);

struct Teammate {
  std::uint8_t id;
  std::string name;
  natives::Ped ped; // local ped standing in for them (puppet)
  bool downed;
};

struct Frame {
  bool in_session = false, hosting = false;
  std::uint8_t my_id = 0;
  std::vector<Teammate> teammates;
};

// Game thread, every frame.
void tick(const Frame& f, net::Session& net);
void on_message(const net::GameMessage& m, std::uint8_t my_id);
// The local player is down (sent in PlayerState flags).
bool local_downed();
// Damage from a remote player while on a mission: never below the downed threshold.
std::uint32_t floor_health(std::uint32_t health);
void clear();

} // namespace ivc::downed
