#pragma once
// Script-drawn mission HUD for joiners (timers, counters, bars).
//
// IV missions draw these themselves every frame: SET_TEXT_* setup calls, then
// DISPLAY_TEXT*(x, y, key, ...) and DRAW_RECT (the engine's on-screen timer is almost
// unused). The host records the calls its story scripts make in a frame (a draw list) and
// sends the latest list ~10 times a second; a joiner re-issues the latest list every frame
// from our tick (drawing must happen every frame, from a script) and drops it when stale.
// Calls after SET_TEXT_RENDER_ID (drawing onto an in-world screen) are not captured.

#include <cstdint>
#include <span>

#include "net/session.h"

namespace ivc::story::hud {

void register_hooks();
void host_tick(bool hosting, net::Session& net);
void client_apply(std::span<const std::uint8_t> msg);
void client_tick();
void clear();

} // namespace ivc::story::hud
