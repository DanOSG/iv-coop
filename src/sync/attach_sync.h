#pragma once
// Attachment sync: when the host's story scripts attach one networked entity to another (ATTACH_OBJECT_TO_PED,
// ATTACH_PED_TO_CAR, ATTACH_CAR_TO_OBJECT, ...) or to the host's player (whose copy on a joiner is the
// puppet standing in for the host), joiners make the same call on their copies with the same bone, offsets
// and rotation, and DETACH_* is replayed the same way. While attached, the child's own state stream is
// paused (EntitySync::set_attached) so the parent carries it; on detach the stream re-places it.
//
// Message: [kMsg][sender][op][hash][argc][child ref][parent ref][argc raw 32-bit argument slots]
// where a ref is [kind: 0 networked entity id, 1 host's player ped][id]; the handle slots in the raw
// arguments are filled in from the refs on the receiving side.

#include <cstdint>
#include <span>

#include "net/session.h"
#include "sync/entity_sync.h"

namespace ivc::attach_sync {

constexpr std::uint8_t kMsg = 53;

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Register before native_hooks::install().
void register_hooks();
// Host, every frame after the story ops ran (new entities are networked by then): sends what the story
// scripts attached/detached since the last call.
void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents);
// Host: a late joiner gets the current attachments (call after the entity creates are resent).
void host_on_player_joined(net::Session& net);
// Joiner: a message from the host.
void on_message(const net::GameMessage& m, sync::EntitySync& ents);
// Joiner, every frame: applies queued attachments once both copies exist.
void client_tick(sync::EntitySync& ents);
// Session ended.
void clear();

} // namespace ivc::attach_sync
