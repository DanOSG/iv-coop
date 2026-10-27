#pragma once
// Ambient world sync (M5b, first slice): city pedestrians and traffic that are near two or more players
// become networked entities, so partners standing together see the same people and cars.
//
// Host: scans for ambient peds and cars within kRegisterM of the host and of a partner, registers them
// (ambient ids, slower stream), and releases them once fewer than two players are near. A car's driver is
// registered with it and follows its owner. Joiner: while the host is close, thins its own local
// population in the overlap zone (density multipliers + deleting off-screen duplicates) so the
// networked copies do not double up. Gated by the "Ambient sync" session setting.

#include <cstdint>
#include <vector>

#include "net/session.h"
#include "sync/entity_sync.h"

namespace ivc::sync::ambient {

// Game thread, every frame (after the session's player positions are known).
void tick(std::uint32_t now_ms, bool in_session, bool hosting, bool enabled, std::uint8_t my_id,
          const std::vector<PlayerPos>& players, EntitySync& ents, net::Session& net);

// How many ambient entities the host currently shares (for logs / overlay).
std::size_t shared_count();

} // namespace ivc::sync::ambient
