#pragma once
// Drunk sync: when the host's scripts make the host's player drunk (friend drinking outings, dates:
// SET_PED_IS_DRUNK on the player ped, and the drunk camera the same scripts set with SET_DRUNK_CAM), every
// partner who is *present for the outing* gets the same - their own player ped drunk (the wobbly walk) and
// their own drunk camera - and it ends when the host's ends.
//
// Present: within kPresentRange (30 m) of the host's player when the host becomes drunk. The host decides
// (it knows every partner's position) and sends a bitmask of player ids with the state; a partner who
// walks up later, or joins mid-outing, is not made drunk. Everyone, present or not, sees the host's copy
// of the host walk drunk. The state is re-sent every few seconds, so a copy that was recreated is fixed.

#include <cstdint>
#include <span>
#include <vector>

#include "game/natives.h"
#include "net/session.h"

namespace ivc::drunk_sync {

constexpr std::uint8_t kMsg = 51;
constexpr float kPresentRange = 30.0f; // metres from the host's player when the drunk state starts

struct Mate {
  std::uint8_t id;
  natives::Ped ped; // the partner's copy on this machine (host side: used for the range check)
};

struct Frame {
  bool in_session = false, hosting = false;
  std::uint8_t my_id = 0;
  std::vector<Mate> mates;
  natives::Ped host_puppet = 0; // joiners: the copy of the host here
};

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Register before native_hooks::install().
void register_hooks();
// Game thread, every frame.
void tick(const Frame& f, net::Session& net);
void on_message(const net::GameMessage& m);

} // namespace ivc::drunk_sync
