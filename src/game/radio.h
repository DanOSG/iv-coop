#pragma once
// Engine access for the radio (docs/research-radio.md, sections 8-10).
//
// Every station plays all the time: each has two stream slots, the current one holding the playing
// item - a (category, track) pair resolved through the station's lists - and a position in ms. Only the
// station you hear streams audio; the others are idle, their position counting with time, and start
// streaming from it when tuned. Stations here are indices into the engine's station array, which is
// NOT the numbering of GET_PLAYER_RADIO_STATION_INDEX / RETUNE_RADIO_TO_STATION_INDEX (player index).

#include <cstdint>

namespace ivc::radio {

struct Item {
  std::uint8_t category = 0xFF;
  std::uint16_t track = 0;
  bool operator==(const Item&) const = default;
};

struct StationState {
  Item item;                 // the current slot's item
  std::uint32_t position = 0; // ms into it
  bool audible = false;       // streaming (the station you hear)
  bool valid = false;
};

int station_count();
StationState read(int station);
// Plays `item` from `offset_ms` on the station's current slot: audibly if it is streaming, otherwise
// loaded and positioned so tuning in starts there. False if the item doesn't resolve.
bool play(int station, Item item, std::uint32_t offset_ms, bool audible);
// Moves an idle station's position (no reload).
void set_idle_position(int station, std::uint32_t offset_ms);

// The player's tuned station, in player numbering (255 = off).
int player_station();
void retune(int player_station);

} // namespace ivc::radio
