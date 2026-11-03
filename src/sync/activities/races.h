#pragma once
// Races adapter extras beyond the common register_hooks()/host_tick(): the grid / go / result message (id 79).
// Wiring: remote_players dispatches messages with races::is_msg(d) to races::on_message(m); every frame (script
// context) call client_tick(my_id) on everyone (partners: build their car on the grid, hold it for the countdown) and,
// on the host, host_send(net).
//
//   [79][sender][kind][target id][...]
//   kind 0 grid   (host -> partner): slot u8, car model u32, x y z heading f32
//   kind 1 go     (host -> partner): the countdown is over
//   kind 2 result (host -> partner): position u8, racers u8, finished u8, n u8, n x (id u8, name)
//   kind 3 cancel (host -> partner): the race ended before a result; let the car go

#include <cstdint>
#include <span>

#include "net/session.h"

namespace ivc::activities::races {

constexpr std::uint8_t kMsg = 79;
inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

void on_message(const net::GameMessage& m);
// my_id: our session id (messages carry their target).
void client_tick(std::uint8_t my_id);
void host_send(net::Session& net);

} // namespace ivc::activities::races
