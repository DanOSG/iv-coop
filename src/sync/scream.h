#pragma once
// The scream key (default J, iv-coop.ini [Fun] ScreamKey): our character lets out one of the game's own
// screams - high fall, on fire or panic, picked at random - and partners hear the same scream from our
// character in their game. 1.5 s cooldown.

#include <cstdint>
#include <filesystem>
#include <span>

#include "game/natives.h"
#include "net/session.h"

namespace ivc::scream {

constexpr std::uint8_t kMsg = 45;

void configure(const std::filesystem::path& ini);
// Game thread, every frame: the key (also outside a session, just locally).
void tick(bool in_session, net::Session& net);
inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }
// A partner screamed: `ped` is the character standing in for them here.
void on_message(const net::GameMessage& m, natives::Ped ped);

} // namespace ivc::scream
