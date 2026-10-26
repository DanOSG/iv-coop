#pragma once
// A script-owned ped standing in for a remote player.
//
// Animated mode (default): the ped walks/runs/sprints under its own animation toward a point
// just ahead of the remote player's interpolated position, and is snapped into place only
// when it falls too far behind. Frozen mode: positioned directly every frame (slides; used
// as a fallback and for debugging the raw sync).
//
// Both are interim: real player-slot peds driven by replicated pad input replace this later.

#include <cstdint>
#include <string>

#include "game/natives.h"
#include "sync/player_state.h"

namespace ivc::sync {

class Puppet {
 public:
  explicit Puppet(std::string model_name = "M_Y_MULTIPLAYER", bool animated = true)
      : model_name_(std::move(model_name)), animated_(animated) {}

  // Game thread, script context. Requests the model and spawns when it's loaded; returns
  // true once the ped exists (respawning it if the game removed it).
  bool ensure(const natives::Vector3& at);
  // Moves the ped toward `target` (animated) or onto it (frozen). Call once per frame.
  void drive(const PlayerState& target, std::uint32_t now_ms);
  void despawn();
  // Downed partner: red, flashing blip; otherwise the normal partner blip.
  void set_downed_marker(bool downed);
  // Ragdoll and fire follow the owner: a copy is ragdoll-locked (hits don't knock it over here) and
  // falls / burns only while its owner does. Call every frame while it's on foot and not downed.
  void mirror_body(bool ragdoll, bool on_fire, std::uint32_t now_ms);
  // Lets the engine ragdoll it (downed pose, death).
  void unlock_ragdoll();
  // Session settings: can our player lock on to this partner; is their map blip shown.
  void apply_settings(bool targetable, bool blip_visible);

  // Seats the puppet in `vehicle` (seat -1 = driver) if it isn't already there; while
  // seated, don't call drive(). Returns true once seated.
  bool sit_in(natives::Vehicle vehicle, std::int8_t seat);
  // Takes the puppet out of any vehicle, onto `at`.
  void leave_vehicle(const natives::Vector3& at);
  bool in_vehicle() const { return vehicle_ != 0; }

  // --- combat (M5) ---
  // The puppet never really loses health: it sits on a large buffer that is refilled every
  // frame. Returns how much damage `attacker` (normally our own player) did to it since
  // the last call, so it can be reported to the player it stands for.
  std::uint16_t take_hits_from(natives::Ped attacker, bool cancel_knockdowns = true);
  // Mirrors the remote player's weapon, shots (fires the difference of their cumulative
  // counter at their aim point), aiming, and death/respawn. Call after drive()/sit_in().
  void apply_combat(const PlayerState& s, std::uint32_t now_ms);
  bool dead() const { return dead_; }
  // Shows the remote player's health (100..200) on the copy's lock-on health bar.
  void show_health(std::uint32_t health);

  // --- appearance (M6d) ---
  // Use this exact model (the remote player's real model, e.g. Niko for the host) instead
  // of the configured fallback; 0 = fallback. A change respawns the puppet.
  void set_model(std::uint32_t model_hash);
  // Wears the remote player's outfit (re-applied whenever it changes).
  void apply_outfit(const PlayerState& s);

  bool spawned() const { return ped_ != 0; }
  // An extra player ped: the engine applies no damage to it from us (team_fire detects our hits).
  bool is_player_ped() const { return ped_ && is_player_ped_; }
  natives::Ped ped() const { return ped_; }
  // Short description of what the puppet is doing, for status displays.
  const char* phase() const { return phase_; }
  float error_m() const { return error_m_; }
  std::uint32_t snaps() const { return snaps_; }

 private:
  enum class Action { None, Standing, Moving, Aiming };

  void drive_animated(const PlayerState& t, std::uint32_t now_ms);
  void snap_to(const PlayerState& t);

  std::string model_name_;
  bool animated_;
  std::uint32_t model_ = 0;
  natives::Ped ped_ = 0;
  natives::Blip blip_ = 0;
  const char* phase_ = "idle";

  // Animated-mode state.
  Action action_ = Action::None;
  natives::MoveState move_ = natives::MoveState::Walk;
  natives::Vector3 last_goal_{};
  float last_heading_ = 0;
  std::uint32_t last_issue_ms_ = 0;
  bool moving_aimed_ = false; // the current movement task also aims
  natives::Vector3 last_aim_{};
  float error_m_ = 0;
  std::uint32_t snaps_ = 0;
  natives::Vehicle vehicle_ = 0;
  std::int8_t seat_ = -1;

  // Appearance state.
  std::uint32_t model_override_ = 0;
  std::uint32_t rejected_model_ = 0;
  bool is_player_ped_ = false; // an extra player slot (player hit reactions) vs an ordinary ped
  bool ragdoll_locked_ = false, mirroring_ragdoll_ = false;
  std::uint32_t ragdoll_since_ = 0;
  std::uint32_t shown_health_ = 2000; // the copy's health as last set (take_hits_from refills to it)
  bool targetable_ = true, blip_visible_ = true; // apply_settings, as last applied to this ped/blip
  bool settings_applied_ = false;              // to this ped yet (a new ped starts in the player group)
  bool downed_marker_ = false; // last model set_model refused (logged once)
  std::uint8_t worn_drawable_[11] = {};
  std::uint8_t worn_texture_[11] = {};
  bool outfit_applied_ = false;

  // Combat state.
  bool dead_ = false;
  std::uint32_t weapon_ = 0;
  std::uint16_t last_shots_ = 0;
  bool have_shots_ = false;
  std::uint32_t last_aim_ms_ = 0;
};

inline constexpr std::uint32_t kPuppetHealthBuffer = 2000;

} // namespace ivc::sync
