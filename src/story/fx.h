#pragma once
// Explosions, fires and script particle effects for joiners (message type 52, host -> all, reliable).
//
// The host's story scripts call ADD_EXPLOSION*, EXPLODE_CAR/EXPLODE_CHAR_HEAD, START_*_FIRE and the PTFX
// natives. Each captured call is sent as a "call record": the native's hash and its raw argument slots, with
// the entity argument (if any) replaced by a networked entity id (or "the host's player") and the name
// argument carried as text. A joiner replays the call on its own copies, so the effect appears at the same
// place. Natives that return a handle (script fires, looped PTFX) are mapped host handle -> joiner handle so
// STOP_PTFX / REMOVE_PTFX / REMOVE_SCRIPT_FIRE (and REMOVE_ALL_SCRIPT_FIRES) can undo them; the live ones are
// resent to late joiners.
//
// Damage stays where it is: the replayed explosion has no attacker, so it is not reported as damage by a
// joiner's combat sync, and followers (copies of host-simulated entities) sit on a refilled health buffer. The
// native list has no explosion variant without damage, so a joiner's own character and any entity the joiner
// itself simulates can still be hurt by the blast locally, which is the intended behaviour of being next to it.

#include <cstdint>
#include <span>

#include "game/natives.h"
#include "net/session.h"
#include "sync/entity_sync.h"

namespace ivc::story::fx {

constexpr std::uint8_t kMsg = 52;

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Registers the capture hooks; call before native_hooks::install().
void register_hooks();
// Host, every frame (also when not hosting: capture off and state dropped).
void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents);
// Host: someone joined; resend the fires and looped particle effects still running.
void host_on_player_joined(std::uint8_t player, net::Session& net);
// Joiner: queue a message from the host.
void client_on_message(const net::GameMessage& m);
// Joiner, every frame: apply queued effects (retrying ones whose entity copy isn't there yet).
// `host_ped`: the character standing in for the host here (0 if none).
void client_tick(sync::EntitySync& ents, natives::Ped host_ped);
// Session ended: remove the effects we replayed.
void clear();

} // namespace ivc::story::fx
