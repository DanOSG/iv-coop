#pragma once

namespace ivc::d3d9_hook {

// Hooks GTAIV.exe's Direct3DCreate9 import so we see the game's device when it is
// created, then detours that device's Present/Reset to drive the overlay.
// Safe to call from DllMain (IAT write only; detours are installed later on the game thread).
bool install();

} // namespace ivc::d3d9_hook
