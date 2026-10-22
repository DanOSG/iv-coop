#pragma once
// Positional dialogue and phone calls for joiners (M6).
//
// Missions rarely call the conversation natives themselves: they post the lines they want
// through globals and the shared speechcontrol script builds and plays them; phone calls
// are started by spcellphonecalling. A conversation is built as
//   NEW_SCRIPTED_CONVERSATION | NEW_MOBILE_PHONE_CALL
//   ADD_LINE_TO_CONVERSATION(speaker, context, subtitle, listener, flag) | ADD_LINE_TO_MOBILE_PHONE_CALL(...)
//   ADD_NEW_CONVERSATION_SPEAKER(slot, ped, voice)
//   START_SCRIPT_CONVERSATION(a, b) | START_MOBILE_PHONE_CALL(ped, voice, ped, voice, a, b)
// The host records the build and, at the start, sends it with each speaker ped resolved
// to "the host's player" or a networked entity. A joiner rebuilds it with the same natives
// but every speaker mapped to its local copy (the host's puppet, the entity's copy), so the
// game plays each line in 3D from that ped. Phone calls play from the host's puppet in both
// voices. Subtitle text blocks are already replicated by story_sync.

#include <cstdint>
#include <functional>
#include <span>

#include "game/natives.h"
#include "net/session.h"
#include "sync/entity_sync.h"

namespace ivc::story::dialogue {

// Capture hooks; call from story::register_hooks (before native_hooks::install()).
void register_hooks();
// Host, every frame: sends conversations completed since the last call.
void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents);
// Client: a Conversation/ConversationAbort message from the host.
void client_apply(std::span<const std::uint8_t> msg, sync::EntitySync& ents);
// Client: the local ped standing in for the host's player (its puppet), or 0.
void set_host_ped_provider(std::function<natives::Ped()> provider);
// Session over: stop anything we are playing.
void clear();

} // namespace ivc::story::dialogue
