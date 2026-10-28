#pragma once
// Scripted-camera scenes for joiners (M7; message type 58, host -> joiners).
//
// When the host's story scripts run an in-engine scene (a script camera made active with SET_CAM_ACTIVE and
// ACTIVATE_SCRIPTED_CAMS), the host streams that camera's position, rotation and FOV at 25 Hz (unreliable),
// read each frame with GET_CAM_POS/ROT/FOV rather than mirroring the CREATE_CAM/POINT_CAM_*/ATTACH_CAM_* calls:
// attachments and interpolation resolve themselves, and a camera on an entity lands wherever the joiner's copy of
// that entity is, because the host's resulting transform is what's sent. A joiner drives a script camera of its
// own to that state (smoothed between samples), with the player's control off for as long as the scene lasts.
// Screen fades the host's scripts do around a scene (DO_SCREEN_FADE_OUT/IN, while a scene is on or player control
// is off) are replayed too; a fade-in always undoes a fade-out we made. The "Scenes" co-op setting picks Watch
// (the default) or Keep control (joiners ignore all of it). Not mirrored: camera shake, depth of field.
//
// The bowling lane's camera (tenpinbowl is a story script like the rest) is streamed too, but only while a bowling
// game with partners is on, and only the host's bowlers apply it (op 3, the payload of op 0).
//
// Messages: [kMsg][sender][op] then
//   op 0 camera : 7 floats x y z rx ry rz fov
//   op 1 end    : (the scene is over)
//   op 3 camera : as op 0, the bowling lane camera
//   op 2 fade   : u8 kind (0 out, 1 in, 2/3 the unhacked variants), u32 ms, u8 scene

#include <cstdint>
#include <span>

#include "net/session.h"

namespace ivc::story::scene_cam {

constexpr std::uint8_t kMsg = 58;

inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }

// Registers the capture hooks; call before native_hooks::install().
void register_hooks();
// Host, every frame (also when not hosting: state dropped).
void host_tick(bool hosting, net::Session& net);
// Joiner: a message from the host.
void client_apply(std::span<const std::uint8_t> msg);
// Every frame; `watch` is the session's Scenes setting. Also runs outside a session to finish a teardown.
void client_tick(bool watch);
// Session over: give the joiner their camera, control and screen back.
void clear();

} // namespace ivc::story::scene_cam
