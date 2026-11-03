#pragma once
// Activity seats: the shared core that lets the host's activity scripts (pool, darts, bowling, golf, air hockey, arm
// wrestling, ...) play a partner as their own character. The host's script stays the game; a partner is "seated" in
// one of its player slots:
//   - host: the partner's puppet ped is handed to the script (the adapter puts it in the script's seat) and is no
//     longer driven by the partner's movement stream (owns()); while the seat is live its position, heading and the
//     animations the script plays on it are sent to that partner;
//   - partner: their own character follows the seat (position, heading, the same animations) with player control
//     off, so everyone sees them playing; while the seat is "active" (their turn, or always for real-time games)
//     their sticks, mouse, keyboard aim, controls and buttons go to the host;
//   - host: the script's input reads are answered with the active seat's input: reads on that seat's pad index
//     (scripts that read a pad per seat, e.g. pool) and, while it's that seat's turn, the generic reads (pad 0 / 2)
//     from that script.
//
//   [75][sender][op][target id][...]
//   op 0 seat (host -> all): on u8, active u8, script name (activity), pad i8
//   op 1 pose (host -> all, ~15 Hz while seated): x y z heading f32
//   op 2 anim (host -> all, reliable): the native's hash, argc, raw args, the dictionary and animation strings
//   op 3 input (partner -> host): controls u32, just u32, buttons u32, mouse buttons u8, controller u8,
//        sticks lx ly rx ry i16, mouse x y i16, keyboard move x y i16
// Adapters (sync/activities) decide who sits where and whose turn it is; this module only moves people and input.

#include <cstdint>
#include <span>
#include <string>

#include "game/natives.h"
#include "net/session.h"

namespace ivc::activity_seats {

constexpr std::uint8_t kMsg = 75;

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// ---- host

// Seat partner `id` (their puppet `ped` here) in `script`'s game. `pad`: the controller index the script reads that
// seat's sticks/buttons from (-1: it only reads the generic pad, so turns decide; see set_active). A pad above 2
// (arm wrestling's 99) makes the seat live all the time: the partner streams input without a turn, and only reads
// on that pad are answered with it; an adapter that reads the input itself (sample/control) can use an unused pad.
void seat(std::uint8_t id, natives::Ped ped, const std::string& script, int pad);
void unseat(std::uint8_t id);
void unseat_all();
// Whose turn it is in the seats' script: a seated partner, or 0xFF for the host. While it's a partner's, that
// script's generic input reads (pad 0 / 2) get the partner's input and its GET_PLAYER_CHAR returns the partner's
// puppet, so the script plays them as "the player" (their own character takes the shot).
void set_active(std::uint8_t id);
// Host: is partner `id` seated? (Real-time games such as air hockey keep every seat live on its own pad.)
bool seated(std::uint8_t id);
// Host: seated partner `id` is sending input (self-test).
bool has_input(std::uint8_t id);
// Host: what a seated partner's devices say now (for adapters that answer a script's mouse / keyboard fallback for
// them): the mouse and keyboard-move reads and whether they use a controller. Idle (fresh false) once they go quiet.
struct PadSample {
  bool fresh = false, controller = false;
  int mx = 0, my = 0, kx = 0, ky = 0;
  int lx = 0, ly = 0, rx = 0, ry = 0; // the sticks (adapters that drive a script's statics from them, e.g. air hockey)
};
PadSample sample(std::uint8_t id);
// Host: partner `id` holds the relayed control (pad, control) (the list in the .cpp; others are never true); with
// `edge`, whether it was pressed since the last call (consumed). For adapters that decide from their input in the
// script's statics rather than through the script's own reads.
bool control(std::uint8_t id, int pad, int control, bool edge = false);
// Host: anyone is seated (story/scene_cam streams the table cameras to the seated players only then).
bool any_seated();
// The host must not drive this puppet from the partner's movement stream (the script has it).
bool owns(natives::Ped ped);
// The script's own opponent ped that a seat replaced (kept, hidden): any DELETE_CHAR / MARK_CHAR_AS_NO_LONGER_NEEDED /
// REMOVE_CHAR_ELEGANTLY the script aims at the seated puppet goes to it instead, so the partner's character survives
// the script's cleanup.
void set_stand_in(std::uint8_t id, natives::Ped stand_in);
// An activity script's statics (the thread's stack, statics first), or nullptr while it isn't running.
std::int32_t* statics(const char* script);

// ---- everyone

// Before native_hooks::install().
void register_hooks();
// Game thread, every frame. `hosting`/`in_session` as everywhere; `my_id` our session id.
void tick(bool in_session, bool hosting, std::uint8_t my_id, net::Session& net);
void on_message(const net::GameMessage& m);
// Partner: we're seated in the host's game (our character follows the seat; story/scene_cam shows its camera).
bool me_seated();

} // namespace ivc::activity_seats
