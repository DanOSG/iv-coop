#pragma once
// Invasions (ARCHITECTURE, M11), Dark Souls style: a group of 1-4 players invades another group's session.
//
// The window (host): the session is "open to invasion" while the host's Co-op Settings allow it and the host is on a
// mission (GET_MISSION_FLAG) or has 3+ wanted stars; it closes 5 s after that stops. While open the ENet session
// admits invaders (net::Session::set_invasion_open) and the Steam lobby advertises it (ivc_open), so the phone's
// Multiplayer > Invade finds it. Invaders are joiners with the Invader role: up to 4 beside the co-op group's 4.
//
// Invaders (client side): they play the host's world like joiners (story scripts frozen) but take no part in the
// host's story (remote_players drops the story messages, no wallet, no downed/revive, no regroup). When their FBI
// agent model is worn they spawn together: the lowest-id invader picks a road node 200-400 m from the target (the
// host's protagonist), creates an FBI car there, and the others warp into its passenger seats. They get an FBI
// loadout and hunt the target; their only blip is the target's (plus their own team's). The invasion ends - they
// leave to their own game, position/health/weapons restored - when the window closes, the target dies (success) or
// they are killed.
//
// Team fire between invaders and the invaded group is always on (hostile()); co-op vs co-op keeps the setting.
//
// Messages (65): [65][sender][sub u8][value u8]. sub 0 = Open (host -> all: the window), 1 = Result (host -> all:
// 1 the target died, 2 the window closed).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game/natives.h"
#include "net/session.h"

namespace ivc::invasion {

constexpr std::uint8_t kMsg = 65;
enum class Sub : std::uint8_t { Open = 0, Result = 1 };
enum class Result : std::uint8_t { TargetDied = 1, WindowClosed = 2 };

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }
// Invaders and the invaded group are enemies whatever the Team fire setting says.
inline bool hostile(net::Role a, net::Role b) { return a != b; }

struct Player {
  std::uint8_t id;
  net::Role role;
  natives::Ped ped = 0;      // the puppet standing in for them here (0 if not spawned)
  natives::Vehicle car = 0;  // the vehicle they're in here (0 on foot / not arrived)
  bool known_pos = false;    // pos is from a received state
  natives::Vector3 pos{};
  bool dead = false;
};

struct Frame {
  bool in_session = false, hosting = false;
  std::uint8_t my_id = 0;
  net::Role my_role = net::Role::Coop;
  bool ready = false;          // invader: the FBI model is worn (client mode is on)
  std::string host_name;
  std::vector<Player> players; // everyone else in the session
};

// Game thread, every frame (host: the window; invader: the spawn and the hunt).
void tick(const Frame& f, net::Session& net);
void on_message(const net::GameMessage& m);

// Host: invaders are being admitted (for the lobby flag).
bool open();
// Invader, on connecting: remember where we were (position, health, weapons) to put them back.
void begin(natives::Ped me);
bool active();
// Invader, session over and our own model back: restore position/health/weapons, delete the car we made.
void finish();
// Invader: the invasion is over; the caller leaves the session and shows `message`.
std::optional<std::string> take_leave();
// The invader model (an FBI agent) if the game has it, else 0.
std::uint32_t invader_model();

} // namespace ivc::invasion
