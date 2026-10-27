#pragma once
// Mission ped looks and script-set object/ped state (message type 54).
//
// A networked ped's copy on a joiner is made from its model alone, so its clothes, face and props would be
// default or random. The host reads each networked ped's actual look with the GET_ natives (component
// drawable/texture, props) - not the arguments of the SET_ calls, since the random variation picks values -
// and sends it whenever it changes; the copy gets it applied as soon as it exists.
//
// Also replayed on the copy: SET_OBJECT_VISIBLE, SET_OBJECT_COLLISION, SET_OBJECT_DYNAMIC (never unfreezes a
// follower), SET_CHAR_VISIBLE and PLAY_OBJECT_ANIM that story scripts call on networked entities.

#include <cstdint>
#include <span>

#include "net/session.h"
#include "sync/entity_sync.h"

namespace ivc::ped_looks {

constexpr std::uint8_t kMsg = 54;

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Registers the capture hooks; call before native_hooks::install().
void register_hooks();
// Every frame (game thread), host or not; hosting=false does nothing on the host side.
void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents);
// Host: someone joined; resend all cached looks and states (call after the entity creates).
void host_on_player_joined(net::Session& net);
// Client: a message from the host.
void on_message(const net::GameMessage& m);
// Client, every frame: apply to copies that have spawned.
void client_tick(sync::EntitySync& ents);
// Session ended.
void clear();

} // namespace ivc::ped_looks
