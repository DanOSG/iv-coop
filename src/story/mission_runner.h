#pragma once
// Unattended mission runner (tools/mission_runner.py drives it). The game is launched once with
// plugins/iv-coop/runner.txt listing missions ("run=<script>,<slot>[,x,y,z]", in order). For each,
// the runner loads that save slot in place, the way the pause menu's Load Game does, and the
// story flow ("main") starts the mission's script, either by itself or when the runner walks into
// the mission's contact point (x,y,z, from main's own tables) or the flow's contact blips. The
// runner hosts a session (the test bot joins), records it, and hands the mission to the audit's
// auto-play. When it ends or times out, a line goes to plugins/iv-coop/runner_result.txt and the
// next save loads; after the last, the game closes. The window is kept running in the background
// and sent behind other windows, so the machine stays usable.
//
// runner.txt is renamed to runner.active at startup: one run per launch, and a normal launch
// afterwards is never taken over.

#include <filesystem>

namespace ivc::story::runner {

// At startup (file IO only).
void configure(const std::filesystem::path& plugin_dir);
// Before native_hooks::install().
void register_hooks();
// Game thread, script context, every frame.
void tick();

} // namespace ivc::story::runner
