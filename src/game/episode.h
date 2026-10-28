#pragma once
// Which game is running: GTA IV, The Lost and Damned or The Ballad of Gay Tony. The Complete Edition runs all three
// from the same GTAIV.exe as "episodes" (GET_CURRENT_EPISODE: 0 IV, 1 TLAD, 2 TBoGT; the decompiled scripts build
// their text and mission keys from it, e.g. EMAILE1, E1M_1). Scripts, statics layouts, protagonist and radio lists
// differ per episode, so everything that reads script memory by fixed index asks here first.
//
// Everyone in a session must be in the same episode (sync/episode_sync, message 67): the host's episode is its
// session's, a joiner in another one is turned away with a message naming the episode to start.

#include <cstdint>

namespace ivc::episode {

enum class Id : std::uint8_t { IV = 0, TLAD = 1, TBoGT = 2, Unknown = 0xFF };

// Game thread, script context, every frame (cheap; the native is re-read every 2 s so a world reload into another
// episode is noticed). Logs the episode, protagonist and radio station count when it is first known or changes.
void tick();

Id current();                    // Unknown until the first tick
inline bool known() { return current() != Id::Unknown; }
inline bool is_iv() { return current() == Id::IV; }  // false while unknown: IV-only features stay off until known

const char* name(Id id);           // "Grand Theft Auto IV", "The Lost and Damned", "The Ballad of Gay Tony"
const char* short_name(Id id);     // "GTA IV", "TLAD", "TBoGT"
const char* protagonist_name(Id id); // "Niko", "Johnny", "Luis"
Id from_wire(std::uint8_t v);       // anything out of range -> Unknown

// The protagonist's ped model. All three episodes use the engine's generic "player" model (peds.ide `player`; the
// episodes only change its clothes), which is the default; if the running game's player turns out to wear another
// real ped model when no session has touched it yet, that measured model is used instead (logged).
std::uint32_t protagonist_model();
// Starts another episode the way the frontend's Episodes menu does (story/self_test): the frontend action handler
// (0x5A3980) runs action 0x3E with the frontend mode at 1, which starts the episode in [0x1160C08] (0x5C1980) and
// rebuilds the world. GTAIV.exe 1.2.0.59, static analysis outside the protected region. Game thread.
void request_switch(Id id);
// Report a model the local player wears while nothing of ours has changed it (not in a session, not previewing the
// character editor). The first report per episode is kept.
void note_player_model(std::uint32_t model);

} // namespace ivc::episode
