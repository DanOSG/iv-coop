#pragma once
#include <windows.h>

struct IDirect3DDevice9;

namespace ivc::overlay {

void configure(bool enabled, int toggle_key, int scripts_key);

// Called from the D3D9 hooks on the game's render thread.
void set_window(HWND window);
void on_present(IDirect3DDevice9* dev);
void on_before_reset();
void on_after_reset();

} // namespace ivc::overlay
