#pragma once
// Scripted NPC animations for joiners (M6).
//
// A mission script that tells one of its networked peds to play an animation or scenario, sit down, put its
// hands up or cower is a task on the host's copy only; the joiners' copies are driven by the movement stream
// and would just stand there. The host captures these natives:
//   TASK_PLAY_ANIM and its _FACIAL / _NON_INTERRUPTABLE / _SECONDARY* / _UPPER_BODY / _WITH_FLAGS /
//   _WITH_ADVANCED_FLAGS / _WITH_FLAGS_AND_START_PHASE variants, TASK_SIT_DOWN*, TASK_START_SCENARIO_IN_PLACE,
//   TASK_USE_NEAREST_SCENARIO_TO_POS(_WARP), TASK_HANDS_UP, TASK_COWER
// and sends the call (ped as its entity id, string arguments as text, the rest as raw slots) so a joiner
// replays the same native on its copy once the animation dictionary is loaded. The host sends End when a
// play-anim stops playing there, Clear when a script clears the ped's tasks. On the joiner the ped's
// puppet leaves the ped alone (Puppet::hold_scripted_anim) until the call ends or the host's ped walks off.
// Task sequences: while a story script has one open (OPEN_SEQUENCE_TASK .. CLOSE_SEQUENCE_TASK) the calls above
// plus a few movement tasks (walk to, turn, stand still, pause) are recorded with ped handle 0 into its recipe;
// TASK_PERFORM_SEQUENCE(_FROM_PROGRESS) on a networked ped sends the recipe (Op::Sequence) and joiners rebuild
// and perform it on their copy. The host sends End when GET_SEQUENCE_PROGRESS on its ped returns -1.

#include <cstdint>
#include <span>

#include "net/session.h"
#include "sync/entity_sync.h"

namespace ivc::story::npc_anims {

constexpr std::uint8_t kMsg = 50;

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Capture hooks; call from story::register_hooks (before native_hooks::install()).
void register_hooks();
// Host, every frame after the frame's entities were registered: sends what was captured.
void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents);
// Host: someone joined; resend the animations still running.
void host_on_player_joined(net::Session& net);
// Client: a message from the host.
void client_apply(std::span<const std::uint8_t> msg, sync::EntitySync& ents);
// Client, every frame: starts animations once the ped copy and the dictionary are there.
void client_tick(sync::EntitySync& ents);
// Session over.
void clear();

} // namespace ivc::story::npc_anims
