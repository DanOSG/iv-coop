#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ivc::script {

struct ThreadInfo {
  std::int32_t id;
  std::string name;
  std::int32_t state; // 0 running, 1 blocked, 2 aborted, 3 halted
  bool suppressed;
};

// Hooks scrThread::Run. Requires game::resolve_addresses() to have succeeded.
bool install();

// Called once per frame on the game thread, inside the host script's context, so
// natives (including entity creation) are safe. Only one callback is supported.
void on_tick(std::function<void()> fn);

// Called once per frame on the game thread *before* any script runs (not in a script
// context: don't call natives). For setup that must precede the scripts, e.g. hooking.
void on_frame_start(std::function<void()> fn);

// Scripts whose context our tick may borrow, best first (default: "main"). The best one
// running is used; if none is, any running script is borrowed until a preferred one runs.
void set_host_scripts(std::vector<std::string> preferred);

// Scripts whose names are in this set are frozen: their Run is skipped. This is the M1
// stand-in for client-side script suppression (M6 blocks them at launch instead).
void suppress(const std::string& name, bool on);
// Freeze every script for which `pred(name)` is true (client mode: all story scripts).
// Frozen scripts keep their state and resume where they were when this is cleared.
// Pass nullptr to clear.
void set_suppress_predicate(std::function<bool(const std::string&)> pred);

// Snapshot of all script threads, refreshed every tick (safe to call from any thread).
std::vector<ThreadInfo> threads();

// Game thread: true while our own tick is running (natives called now are ours, not a
// story script's).
bool in_own_tick();
// Game thread: name of the script whose context is active (the caller of a native), or
// empty outside any script.
std::string current_script_name();
// Game thread: id of that script's thread (0 outside any script). Ids are never reused,
// so a script restarting (e.g. "main" on a save load) shows up as a new id.
std::int32_t current_thread_id();
// Name of the current host script, or empty if none is running yet.
std::string host_script();

} // namespace ivc::script
