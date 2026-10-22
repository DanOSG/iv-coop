#pragma once
// Mission audit harness (M8 groundwork). Ctrl+Shift+A starts or stops it.
//
// For each mission script (found by probing likely names with DOES_SCRIPT_EXIST, or the
// [Audit] Missions list in iv-coop.ini) it loads and starts the script, watches every
// hooked native the mission (and the scripts it starts) calls for a while, nudges it on
// by teleporting the player to its newest blip, then terminates it, cleans up what it
// created, and writes plugins/iv-coop/audit/<mission>.md. Missions can't be passed this
// way (only their own scripts decide that); this captures their setup and early flow.
// Progress is kept in audit/done.txt so a crash or stop resumes where it left off.
//
// Use a throwaway save: missions started out of order can change story state.

#include <string>
#include <vector>

namespace ivc::story::audit {

struct Settings {
  std::vector<std::string> missions; // empty: discover
  unsigned seconds = 150;            // per mission
  unsigned stack = 8192;             // START_NEW_SCRIPT stack size (main uses 8192 for missions)
  bool skip_cutscenes = false;       // CLEAR_CUTSCENE; can crash (see mission_audit.cpp)
};

void configure(const Settings& s);
// Before native_hooks::install().
void register_hooks();
// Game thread, script context, every frame.
void tick();

} // namespace ivc::story::audit
