#pragma once
#include <cstdint>
#include <optional>

#include "sync/player_state.h"

namespace ivc::sync {

// Reads the local player's ped into a snapshot stamped with `now_ms`. Game thread, script
// context. Empty while there is no player ped (menus, loading).
std::optional<PlayerState> capture_local(std::uint32_t now_ms);

} // namespace ivc::sync
