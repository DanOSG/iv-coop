#pragma once
// Story sounds for joiners (M6): what the host's mission scripts play, joiners hear.
//
// The host hooks the sound natives its story scripts call (positional PLAY_SOUND_FROM_*,
// PLAY_AUDIO_EVENT*, PLAY_FIRE_SOUND_FROM_POSITION, TRIGGER_MISSION_COMPLETE_AUDIO, the
// script music stream) and sends each as one small reliable message (type 57). A joiner
// replays it with the same native, entities mapped to their local copies (the host's own
// ped -> the host's puppet; a ped/car/object -> its networked copy; otherwise the position
// the host saw).
//
// Sound ids are script handles (GET_SOUND_ID / RELEASE_SOUND_ID): the host's id is sent as
// is and the joiner allocates its own id the first time it sees one, then maps it so
// STOP_SOUND / RELEASE_SOUND_ID act on the right sound. A script id of -1 (fire and
// forget) stays -1.
//
// Frontend policy: PLAY_SOUND_FRONTEND / PLAY_SOUND / PLAY_AUDIO_EVENT are 2D. They are
// forwarded (a mission stinger or checkpoint chime should reach everyone) except names that
// are UI feedback of the host's own menus: police computer, PC/web browsing, phone keypad
// and SMS, wardrobe, and anything FRONTEND_MENU*. Scripts configured as local never send.
// Joiners play every positional sound regardless of distance (the game's own attenuation
// does the rest).

#include <cstdint>
#include <functional>
#include <span>

#include "game/natives.h"
#include "net/session.h"
#include "sync/entity_sync.h"

namespace ivc::story::sounds {

constexpr std::uint8_t kMsg = 57;

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Capture hooks; call from story::register_hooks (before native_hooks::install()).
void register_hooks();
// Host, every frame: sends what the story scripts played since the last call.
void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents);
// Client: a sound message from the host.
void client_apply(std::span<const std::uint8_t> msg, sync::EntitySync& ents);
// Client, every frame: finishes a preloaded music stream.
void client_tick();
// Client: the local ped standing in for the host's player (its puppet), or 0.
void set_host_ped_provider(std::function<natives::Ped()> provider);
// Session over: stop and release what we are playing.
void clear();

} // namespace ivc::story::sounds
