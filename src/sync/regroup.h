#pragma once
// Regroup: partners are brought to the host when the host's story moves on without them, so scripted
// sequences don't leave them across the map. The host notices a sequence boundary (its character is
// moved by a script: a jump of more than 40 m in a frame; the screen fades back in; a mission starts)
// and, 1.5 s later when things have settled, tells everyone where it is and whether it's driving. Each
// joiner further than 25 m (or on foot while the host drives) is then put:
//   - host in a vehicle: in a free seat of the host's car, else in their own car (theirs, or a new one of
//     the host's model) on the nearest road node behind the host;
//   - host on foot: next to the host on safe ground (collision loaded, GET_SAFE_POSITION_FOR_CHAR, else
//     the ground under the spot), each partner at their own offset, facing the host's way.
// General on purpose: no per-mission knowledge. Missions that need more get specific fixes later.

#include <cstdint>
#include <span>

#include "game/natives.h"
#include "net/session.h"

namespace ivc::regroup {

constexpr std::uint8_t kMsg = 63;

struct Frame {
  bool in_session = false;
  bool hosting = false;
  std::uint8_t my_id = 0;
  natives::Ped host_puppet = 0; // joiners: our copy of the host's character
};

// Game thread, every frame.
void tick(const Frame& f, net::Session& net);
inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }
void on_message(const net::GameMessage& m);
void clear();

} // namespace ivc::regroup
