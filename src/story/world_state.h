#pragma once
// World-state ledger (progression sync).
//
// Story scripts set persistent world state through natives: island/bridge locks are road
// switches made by "main" on every save load, plus car generators, doors, IPLs, safehouses,
// zone settings and service switches. Every machine records its own story scripts' calls
// here, keyed by what they affect (a road box, a door, an IPL...), so a later call replaces
// an earlier one; the ledger resets when a save loads.
//
// A joiner's story scripts are frozen, so its world keeps its own save's state. On the
// first host world op of a session the joiner undoes its own ledger, then applies the
// host's (sent in full on join, then as it changes). On leaving it undoes the host's
// ops and re-applies its own.

#include <cstdint>
#include <span>
#include <vector>

namespace ivc::story::world {

// Capture hooks; call from story::register_hooks (before native_hooks::install()).
void register_hooks();

// Game thread, every frame (detects save loads).
void tick();

// Host: encoded ops recorded since the last call (send them while hosting).
std::vector<std::vector<std::uint8_t>> take_outbox();
// Host: the whole ledger, encoded (for a late joiner).
std::vector<std::vector<std::uint8_t>> encode_ledger();
// Client: apply one host op (StoryMsg::World payload).
void client_apply(std::span<const std::uint8_t> msg);
// Client: session over; put our own world back.
void client_restore();

std::size_t own_size();

} // namespace ivc::story::world
