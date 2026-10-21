#include "game/native_hooks.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

#include "core/log.h"
#include "game/script.h"

namespace ivc::native_hooks {
namespace {

using Handler = void(__cdecl*)(natives::CallContext*);

constexpr int kMaxHooks = 192;

struct Slot {
  std::uint32_t hash = 0;
  const char* name = "";
  void* target = nullptr;
  Handler orig = nullptr;
  std::vector<Callback> before;    // run before the original
  std::vector<Callback> callbacks; // run after it
};

std::array<Slot, kMaxHooks> g_slots;
int g_count = 0;
bool g_installed = false;
bool g_in_callback = false; // natives called from a callback are ours: don't report them
bool g_trace = false;       // log each hooked call *before* it runs (crash bisection)

constexpr int kMaxArgs = 16;

// Runs a slot's callbacks (C++ objects live here, so it can't hold the SEH guard itself).
__declspec(noinline) void run_callbacks(Slot& s, natives::CallContext* ctx, const std::uint32_t* args, int argc,
                                        bool pre) {
  const Call call{ctx, s.hash, s.name, script::current_script_name(), args, argc};
  for (const auto& cb : pre ? s.before : s.callbacks) {
    try {
      cb(call);
    } catch (const std::exception& e) {
      log::error("Native hook {} callback threw: {}", s.name, e.what());
    }
  }
}

// A callback that faults (e.g. a native whose community-documented signature is wrong, so
// an "argument" isn't what we think) must never take the game down: catch the hardware
// exception here, remember which native did it, and carry on.
bool g_faulted[kMaxHooks] = {};
void guarded_callbacks(int n, natives::CallContext* ctx, const std::uint32_t* args, int argc, bool pre) {
  __try {
    run_callbacks(g_slots[n], ctx, args, argc, pre);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_faulted[n] = true;
  }
}

// One detour per slot, so each knows which native it is.
template <int N>
void __cdecl detour(natives::CallContext* ctx) {
  Slot& s = g_slots[N];
  const bool report = !g_in_callback && !script::in_own_tick();
  if (g_trace && report) log::debug("[trace] -> {} from {}", s.name, script::current_script_name());
  // Snapshot the arguments before the native overwrites slot 0 with its result.
  std::uint32_t args[kMaxArgs];
  const int argc = report ? std::clamp(ctx->arg_count, 0, kMaxArgs) : 0;
  if (argc) std::memcpy(args, ctx->args, static_cast<std::size_t>(argc) * 4);
  if (report && !s.before.empty()) {
    g_in_callback = true;
    guarded_callbacks(N, ctx, args, argc, true);
    g_in_callback = false;
  }
  s.orig(ctx);
  if (!report || s.callbacks.empty()) return;
  g_in_callback = true;
  guarded_callbacks(N, ctx, args, argc, false);
  g_in_callback = false;
  if (g_faulted[N]) {
    g_faulted[N] = false;
    log::error("Native hook {}: a callback faulted (bad argument assumption?); call skipped", s.name);
  }
}

template <std::size_t... I>
constexpr std::array<Handler, sizeof...(I)> make_detours(std::index_sequence<I...>) {
  return {&detour<static_cast<int>(I)>...};
}
constexpr auto kDetours = make_detours(std::make_index_sequence<kMaxHooks>{});

} // namespace

void set_trace(bool on) { g_trace = on; }

bool readable_cstr(const char* p, std::size_t max) {
  if (reinterpret_cast<std::uintptr_t>(p) < 0x10000) return false;
  std::size_t checked = 0;
  while (checked < max) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p + checked, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mbi.Protect & 0xEE)) // any readable protection
      return false;
    const auto region_end = reinterpret_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
    for (const char* q = p + checked; q < region_end && checked < max; ++q, ++checked)
      if (*q == '\0') return true;
  }
  return false;
}

std::string Call::str(int i) const {
  const char* p = arg<const char*>(i);
  if (!p) return "(null)";
  if (!readable_cstr(p)) return std::format("(bad ptr 0x{:X})", reinterpret_cast<std::uintptr_t>(p));
  return p;
}

namespace {
Slot* slot_for(std::uint32_t hash, const char* name) {
  for (int i = 0; i < g_count; ++i)
    if (g_slots[i].hash == hash) return &g_slots[i];
  if (g_count == kMaxHooks) {
    log::error("Native hooks full; {} not hooked", name);
    return nullptr;
  }
  if (g_installed) log::warn("Native hook {} registered after install(); it won't be active", name);
  Slot& s = g_slots[g_count++];
  s.hash = hash, s.name = name;
  return &s;
}
} // namespace

void on(std::uint32_t hash, const char* name, Callback cb) {
  if (Slot* s = slot_for(hash, name)) s->callbacks.push_back(std::move(cb));
}

void before(std::uint32_t hash, const char* name, Callback cb) {
  if (Slot* s = slot_for(hash, name)) s->before.push_back(std::move(cb));
}

bool install() {
  if (g_installed) return true;
  g_installed = true;
  int ok = 0;
  for (int i = 0; i < g_count; ++i) {
    Slot& s = g_slots[i];
    s.target = natives::handler(s.hash);
    if (!s.target) {
      log::error("Native hook {}: handler for 0x{:08X} not found", s.name, s.hash);
      continue;
    }
    bool shared = false;
    for (int j = 0; j < i; ++j)
      if (g_slots[j].target == s.target) shared = true;
    if (shared) { // two natives share one handler: can't tell them apart at the handler
      log::warn("Native hook {}: handler {} shared with another hooked native; skipped", s.name, s.target);
      continue;
    }
    MH_STATUS st = MH_CreateHook(s.target, reinterpret_cast<void*>(kDetours[i]), reinterpret_cast<void**>(&s.orig));
    if (st == MH_OK) st = MH_EnableHook(s.target);
    if (st != MH_OK) {
      log::error("Native hook {} at {} failed: {}", s.name, s.target, MH_StatusToString(st));
      continue;
    }
    ++ok;
  }
  log::info("Native hooks: {} of {} installed", ok, g_count);
  return true;
}

} // namespace ivc::native_hooks
