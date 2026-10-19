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

// Scripts whose context our tick may borrow, best first (default: "main"). The best one
// running is used; if none is, any running script is borrowed until a preferred one runs.
void set_host_scripts(std::vector<std::string> preferred);

// Scripts whose names are in this set are frozen: their Run is skipped. This is the M1
// stand-in for client-side script suppression (M6 blocks them at launch instead).
void suppress(const std::string& name, bool on);

// Snapshot of all script threads, refreshed every tick (safe to call from any thread).
std::vector<ThreadInfo> threads();
// Name of the current host script, or empty if none is running yet.
std::string host_script();

} // namespace ivc::script
