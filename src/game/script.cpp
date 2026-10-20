#include "game/script.h"

#include <MinHook.h>

#include <chrono>
#include <climits>
#include <cstring>
#include <format>
#include <mutex>
#include <set>

#include "core/log.h"
#include "game/addresses.h"

namespace ivc::script {
namespace {

// rage::scrThread field offsets (1.2.0.59). Confirmed from scrThread::Run (state @0x0C,
// id @0x04) and scrThread::Kill (abort reason @0x6C, name follows).
constexpr std::size_t kOffId = 0x04;
constexpr std::size_t kOffState = 0x0C;
constexpr std::size_t kOffName = 0x70;
constexpr std::size_t kNameLen = 24;

template <class T>
T field(const game::scrThread* t, std::size_t off) {
  return *reinterpret_cast<const T*>(reinterpret_cast<const std::uint8_t*>(t) + off);
}
std::string name_of(const game::scrThread* t) {
  const char* p = reinterpret_cast<const char*>(t) + kOffName;
  return std::string(p, strnlen(p, kNameLen));
}
bool is_live(const game::scrThread* t) {
  return t && field<std::int32_t>(t, kOffId) != 0 && field<std::int32_t>(t, kOffState) <= 1;
}

using Run_t = std::int32_t(__fastcall*)(game::scrThread*, void*, std::int32_t);
using UpdateAll_t = bool(__cdecl*)(std::int32_t);
Run_t o_Run = nullptr;
UpdateAll_t o_UpdateAll = nullptr;

// Game-thread state (touched only inside the hooks).
std::function<void()> g_tick;
std::vector<std::string> g_host_prefs = {"main"}; // best first
game::scrThread* g_host = nullptr;
std::string g_host_name;
unsigned long long g_frames = 0;
std::chrono::steady_clock::time_point g_last_heartbeat;

std::mutex g_mutex; // guards the members below (read from the render thread)
std::set<std::string> g_suppressed;
std::function<bool(const std::string&)> g_suppress_pred; // game thread only

bool is_suppressed_locked(const std::string& name) {
  return g_suppressed.contains(name) || (g_suppress_pred && g_suppress_pred(name));
}
std::vector<ThreadInfo> g_snapshot;
std::string g_host_snapshot;

// Rebuilds the thread snapshot and picks this frame's host: the best-ranked preferred
// script that is live, else the live thread with the lowest id (the longest-lived one).
void scan_threads() {
  std::vector<ThreadInfo> list;
  game::scrThread* best = nullptr;
  std::size_t best_rank = SIZE_MAX;
  std::int32_t best_id = INT_MAX;

  const game::ThreadArray* arr = game::g_addr.threads;
  for (std::uint16_t i = 0; arr && arr->data && i < arr->count; ++i) {
    game::scrThread* t = arr->data[i];
    if (!t || field<std::int32_t>(t, kOffId) == 0) continue;
    std::string name = name_of(t);
    const std::int32_t id = field<std::int32_t>(t, kOffId);
    if (is_live(t)) {
      std::size_t rank = g_host_prefs.size();
      for (std::size_t r = 0; r < g_host_prefs.size(); ++r)
        if (g_host_prefs[r] == name) rank = r;
      if (rank < best_rank || (rank == best_rank && id < best_id)) best = t, best_rank = rank, best_id = id;
    }
    list.push_back({id, std::move(name), field<std::int32_t>(t, kOffState), false});
  }

  if (best != g_host) {
    const std::string name = best ? name_of(best) : std::string();
    if (!best)
      log::warn("No live script to host the tick; paused");
    else if (best_rank < g_host_prefs.size())
      log::info("Tick host is now script '{}' (id {})", name, best_id);
    else
      log::warn("No preferred tick host running; borrowing '{}' (id {})", name, best_id);
    if (best) {
      std::string names;
      for (const auto& ti : list) names += std::format("{}({}) ", ti.name, ti.id);
      log::info("{} script threads: {}", list.size(), names);
    }
    g_host = best;
    g_host_name = name;
  }

  std::lock_guard lock(g_mutex);
  for (auto& ti : list) ti.suppressed = is_suppressed_locked(ti.name);
  g_snapshot = std::move(list);
  g_host_snapshot = g_host_name;
}

bool g_in_own_tick = false;

void run_tick(game::scrThread* host) {
  game::scrThread** cur = game::g_addr.current_thread;
  game::scrThread* prev = *cur;
  *cur = host; // natives see the host script as the caller
  g_in_own_tick = true;
  try {
    if (g_tick) g_tick();
  } catch (const std::exception& e) {
    log::error("Tick threw: {}", e.what());
  }
  g_in_own_tick = false;
  *cur = prev;
}

// Once per frame on the game thread, after every script has had its turn.
bool __cdecl hk_UpdateAll(std::int32_t budget) {
  const bool result = o_UpdateAll(budget);
  ++g_frames;
  scan_threads();
  if (g_host) run_tick(g_host);

  const auto now = std::chrono::steady_clock::now();
  if (now - g_last_heartbeat >= std::chrono::seconds(10)) {
    g_last_heartbeat = now;
    std::size_t n;
    {
      std::lock_guard lock(g_mutex);
      n = g_snapshot.size();
    }
    log::info("Heartbeat: frame {}, {} scripts, tick host '{}'", g_frames, n, g_host_name);
  }
  return result;
}

// Per script, per frame. Only used to freeze suppressed scripts.
std::int32_t __fastcall hk_Run(game::scrThread* t, void* edx, std::int32_t budget) {
  bool suppressed = false;
  {
    std::lock_guard lock(g_mutex);
    if (!g_suppressed.empty() || g_suppress_pred) suppressed = is_suppressed_locked(name_of(t));
  }
  return suppressed ? field<std::int32_t>(t, kOffState) : o_Run(t, edx, budget);
}

bool detour(void* target, void* hook, void** orig, const char* name) {
  MH_STATUS st = MH_CreateHook(target, hook, orig);
  if (st == MH_OK) st = MH_EnableHook(target);
  if (st != MH_OK) {
    log::error("Hooking {} failed: {}", name, MH_StatusToString(st));
    return false;
  }
  log::info("Hooked {} at {}", name, target);
  return true;
}

} // namespace

bool install() {
  const auto& a = game::g_addr;
  if (!a.thread_run || !a.update_all || !a.current_thread || !a.threads) return false;
  return detour(a.update_all, reinterpret_cast<void*>(&hk_UpdateAll), reinterpret_cast<void**>(&o_UpdateAll),
                "scrThread::UpdateAll") &&
         detour(a.thread_run, reinterpret_cast<void*>(&hk_Run), reinterpret_cast<void**>(&o_Run), "scrThread::Run");
}

void on_tick(std::function<void()> fn) { g_tick = std::move(fn); }

void set_host_scripts(std::vector<std::string> preferred) {
  if (!preferred.empty()) g_host_prefs = std::move(preferred);
}

void suppress(const std::string& name, bool on) {
  std::lock_guard lock(g_mutex);
  if (on ? g_suppressed.insert(name).second : g_suppressed.erase(name) > 0)
    log::info("Script '{}' {}", name, on ? "suppressed" : "released");
}

void set_suppress_predicate(std::function<bool(const std::string&)> pred) {
  std::lock_guard lock(g_mutex);
  g_suppress_pred = std::move(pred);
  log::info("Script suppression rule {}", g_suppress_pred ? "set" : "cleared");
}

std::vector<ThreadInfo> threads() {
  std::lock_guard lock(g_mutex);
  return g_snapshot;
}

bool in_own_tick() { return g_in_own_tick; }

std::string current_script_name() {
  const game::scrThread* t = game::g_addr.current_thread ? *game::g_addr.current_thread : nullptr;
  return t ? name_of(t) : std::string();
}

std::string host_script() {
  std::lock_guard lock(g_mutex);
  return g_host_snapshot;
}

} // namespace ivc::script
