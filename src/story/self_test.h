#pragma once
// Unattended episode self-test (tools/self_test.py drives it). The game is launched with plugins/iv-coop/selftest.txt
// ("episodes=0,1,2"); for each episode in turn it:
//   1. switches to it the way the frontend's Episodes menu does (episode::request_switch) and lets the world settle;
//   2. opens the phone (a short "up" press for the phone scripts), checks the Multiplayer item is in the main menu
//      and that picking it shows the co-op items, then closes it;
//   3. hosts, and waits for the test bot to join and stay;
//   4. joins a bot host playing the same episode: stays, client mode swaps the player model, leaving restores it;
//   5. joins a bot host playing another episode: is turned away (message 67).
// The bot is started and stopped by the driver: the test writes what it needs into selftest_state.txt ("bot=none",
// "bot=join", "bot=serve <episode>"). Each check appends "<episode>\t<check>\tPASS|FAIL\t<detail>" to
// selftest_result.txt; after the last episode the game closes. selftest.txt is renamed to selftest.active at startup.

#include <filesystem>

namespace ivc::story::self_test {

void configure(const std::filesystem::path& plugin_dir);
// Before native_hooks::install().
void register_hooks();
// Game thread, script context, every frame.
void tick();

} // namespace ivc::story::self_test
