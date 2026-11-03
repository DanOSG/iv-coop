#pragma once
// Darts adapter extras beyond the common register_hooks()/host_tick(): the scoreboard message (id 77).
// Wiring: remote_players dispatches messages with darts::is_msg(d) to darts::on_message(m); every frame (script
// context) call client_tick() on everyone (draws the host's board on partners) and, on the host, host_send(net).

#include <cstdint>
#include <span>

#include "net/session.h"

namespace ivc::activities::darts {

constexpr std::uint8_t kMsg = 77;
inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

void on_message(const net::GameMessage& m);
void client_tick();
void host_send(net::Session& net);

} // namespace ivc::activities::darts
