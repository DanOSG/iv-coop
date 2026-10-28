#pragma once
// Radio sync: everyone hears the same thing on every station - songs, idents, DJ talk, ads, news,
// weather and talk-station segments - at the same position.
//
// The host's stations are the timeline. The host sends every station's current item (category, track)
// and position as soon as any station moves to a new item, and a full keyframe every 3 s. A joiner
// corrects each station when its item differs or its position is off by more than 1 s (0.5 s for a
// silent station): the station it hears restarts on the host's item at the host's position; silent
// stations are loaded and positioned, so tuning in starts in sync.
//
// The station: each player sends the station they have on. In a car, as in GTA Online, only the front
// seats choose: the car plays whichever front-seat player (driver or front passenger) changed it last
// (the driver by default), and back-seat passengers are kept on it (a change from the back is undone).

#include <cstdint>
#include <span>

#include "game/natives.h"
#include "net/session.h"

namespace ivc::radio_sync {

constexpr std::uint8_t kMsg = 43;

struct Frame {
  bool in_session = false, hosting = false;
  std::uint8_t my_id = 0;
  std::uint8_t driver_id = 0xFF; // the player driving the vehicle we're a passenger in (0xFF: none)
  std::uint8_t front_id = 0xFF;  // the other player in the front passenger seat (0xFF: none)
  bool in_car = false;
  bool front_seat = false;       // we drive or sit in the front passenger seat
};

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Game thread, every frame.
void tick(const Frame& f, net::Session& net);
void on_message(const net::GameMessage& m);
void clear();

} // namespace ivc::radio_sync
