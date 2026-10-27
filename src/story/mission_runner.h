#pragma once
// Unattended mission runner (tools/mission_runner.py drives it): the game is launched on a
// save made just before a mission, with plugins/iv-coop/runner.txt saying which one
// (mission=<script>, seconds=<limit>). The game loads that save by itself and the story flow
// ("main") starts the mission's script, which waits at its own blip. The runner hosts a
// session (the test bot joins), records it, hands the mission to the audit (it nudges the
// player through the objectives and skips cutscenes), and when the mission ends or times out
// writes plugins/iv-coop/runner_result.txt and closes the game. The window is kept running
// in the background and sent behind other windows, so the machine stays usable.
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
