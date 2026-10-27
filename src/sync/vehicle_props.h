#pragma once
// Vehicle state sync for networked vehicles: what a vehicle looks like and what has happened to it,
// so every player's copy matches.
//
//   - Appearance: colours, extra colours, livery, extras, dirt. The host reads the real values off its
//     vehicle once it exists (scripts and the game pick them at random) and sends them again if they
//     change; late joiners get them with the rest of the host's state.
//   - Script calls: the host's story scripts opening/shutting/breaking doors, locking, smashing windows,
//     bursting/fixing tyres, FIX_CAR, siren, lights, hazard/indicators, taxi lights, livery, colours,
//     visibility, extras are captured and mirrored on every other copy.
//   - Damage: siren, taxi light, burst tyres, smashed windows and damaged doors are read off the
//     vehicle's authoritative copy (the host's, or the sim owner's if that is a client) and
//     sent when they change.
//
// Message (type 55): [type][sender][kind: 0 appearance, 1 state, 2 op][entity id u32] + payload.

#include <cstdint>
#include <span>

#include "net/session.h"
#include "sync/entity_sync.h"

namespace ivc::vehicle_props {

constexpr std::uint8_t kMsg = 55;

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Capture hooks; call before native_hooks::install().
void register_hooks();
// Game thread, every frame.
void tick(bool in_session, bool hosting, sync::EntitySync& ents, net::Session& net);
void on_message(const net::GameMessage& m);
// Host: someone joined; resend everything we know.
void host_on_player_joined(net::Session& net);
void clear();

} // namespace ivc::vehicle_props
