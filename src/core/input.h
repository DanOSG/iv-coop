#pragma once
#include <windows.h>

namespace ivc::input {

// True while one of this process's windows is in the foreground, so hotkeys don't fire
// while the player is typing in another program.
inline bool game_focused() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
}

// Rising-edge hotkey detector. Initialise `was_down` to true so a key held (or a stale
// state) at startup never fires.
inline bool pressed(int vk, bool& was_down) {
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool edge = down && !was_down && game_focused();
  was_down = down;
  return edge;
}

// Same, for Ctrl+Shift+<vk> (debug/tuning chords that must not collide with game keys).
inline bool pressed_ctrl_shift(int vk, bool& was_down) {
  const bool down = (GetAsyncKeyState(vk) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                    (GetAsyncKeyState(VK_SHIFT) & 0x8000);
  const bool edge = down && !was_down && game_focused();
  was_down = down;
  return edge;
}

} // namespace ivc::input
