#pragma once
// Keep the game running while its window is in the background.
//
// IV pauses (no script ticks, no frames) when it gets told it lost focus. While enabled,
// the focus-loss messages (WM_ACTIVATEAPP off, WM_ACTIVATE inactive, WM_KILLFOCUS) are
// swallowed before the game's window procedure sees them. Only useful in windowed mode:
// a fullscreen device can't render behind other windows. Used by the mission audit so it
// can run unattended while the PC is in use.

namespace ivc::focus {

// Any thread. Finds and subclasses the game window on first enable.
void keep_running(bool on);
bool keeping_running();

} // namespace ivc::focus
