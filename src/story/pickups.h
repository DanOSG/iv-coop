#pragma once
// Story pickups (M6): the pickups the host's story scripts create - weapons, health, armour, cash,
// mission items such as packages and briefcases - exist at the same place for every joiner.
//
// Collection semantics:
//  * The host's script is the authority. A pickup the host's script polls (HAS_PICKUP_BEEN_COLLECTED) is
//    mission-critical: when any joiner collects their copy they tell the host, and from then on the host's
//    script is told the pickup was collected (HAS_PICKUP_BEEN_COLLECTED / DOES_PICKUP_EXIST are answered for
//    it); the host's own copy is removed, and so are the other joiners' copies.
//  * Any pickup the host collects (or its script removes) disappears for every joiner.
//  * A pickup the script never polls (ambient weapon/health/armour/cash) is collected per machine: the joiner
//    gets the effect on their own machine only, nothing else changes.
// Message type 56: [56][sender][op][Spec] with op Create | Remove | Collected; host -> all, except Collected
// (joiner -> host).

#include <cstdint>
#include <span>

#include "game/natives.h"
#include "net/session.h"

namespace ivc::story::pickups {

constexpr std::uint8_t kMsg = 56;

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Capture hooks; call from story::register_hooks (before native_hooks::install()).
void register_hooks();
// Game thread, every frame (host: captured pickups; joiner: copies and collection reports).
void tick(bool hosting, net::Session& net);
void on_message(const net::GameMessage& m, bool hosting);
// A late joiner gets every pickup the host's scripts currently have.
void host_on_player_joined(std::uint8_t player, net::Session& net);
// The joiner's copy of the host's pickup `id` (0 if there is none yet), for blips.
natives::Handle local_handle(std::uint32_t id);
void clear();

} // namespace ivc::story::pickups
