#pragma once
// Episode check for a session (message 67). GTA IV, The Lost and Damned and The Ballad of Gay Tony run from the same
// GTAIV.exe with different scripts, maps, protagonist and radio lists, so every player in a session must be in the
// same episode: the host's is the session's.
//
//   [67][sender][op][episode][protagonist model u32]   (reliable)
//   op 0, host -> everyone: this session's episode, sent whenever someone joins. A joiner in another episode leaves
//          at once and the phone says which episode to start.
//   op 1, joiner -> host: the joiner's episode, sent on connecting. The host only warns (a joiner that doesn't
//          leave by itself, e.g. an older build, is still in the session).
// The Hello/Welcome handshake is untouched (no protocol bump).

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "game/episode.h"
#include "net/session.h"

namespace ivc::episode_sync {

constexpr std::uint8_t kMsg = 67;

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Host: a player joined; tell the session which episode this is.
void host_on_player_joined(net::Session& net);
// Joiner: just connected; tell the host which episode we are in.
void on_connected(net::Session& net);
void on_message(const net::GameMessage& m, bool hosting);
// Joiner: set when the host's episode differs from ours; the text is for the phone. The caller leaves the session.
std::optional<std::string> take_refusal();
// The host's episode while joined (Unknown if not told yet), for logs and displays.
episode::Id host_episode();
// How many times a host in another episode turned us away (self-test).
unsigned refusals();
void clear();

} // namespace ivc::episode_sync
