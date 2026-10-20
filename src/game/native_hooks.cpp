#include "game/native_hooks.h"

#include <MinHook.h>

#include <array>
#include <utility>
#include <vector>

#include "core/log.h"
#include "game/script.h"

namespace ivc::native_hooks {
namespace {

using Handler = void(__cdecl*)(natives::CallContext*);

constexpr int kMaxHooks = 96;

struct Slot {
  std::uint32_t hash = 0;
  const char* name = "";
  void* target = nullptr;
  Handler orig = nullptr;
  std::vector<Callback> callbacks;
};

std::array<Slot, kMaxHooks> g_slots;
int g_count = 0;
bool g_installed = false;
bool g_in_callback = false; // natives called from a callback are ours: don't report them

// One detour per slot, so each knows which native it is.
template <int N>
void __cdecl detour(natives::CallContext* ctx) {
  Slot& s = g_slots[N];
  s.orig(ctx);
  if (g_in_callback || script::in_own_tick()) return;
  g_in_callback = true;
  const Call call{ctx, s.hash, s.name, script::current_script_name()};
  for (const auto& cb : s.callbacks) {
    try {
      cb(call);
    } catch (const std::exception& e) {
      log::error("Native hook {} callback threw: {}", s.name, e.what());
    }
  }
  g_in_callback = false;
}

template <std::size_t... I>
constexpr std::array<Handler, sizeof...(I)> make_detours(std::index_sequence<I...>) {
  return {&detour<static_cast<int>(I)>...};
}
constexpr auto kDetours = make_detours(std::make_index_sequence<kMaxHooks>{});

} // namespace

void on(std::uint32_t hash, const char* name, Callback cb) {
  for (int i = 0; i < g_count; ++i) {
    if (g_slots[i].hash == hash) {
      g_slots[i].callbacks.push_back(std::move(cb));
      return;
    }
  }
  if (g_count == kMaxHooks) {
    log::error("Native hooks full; {} not hooked", name);
    return;
  }
  Slot& s = g_slots[g_count++];
  s.hash = hash, s.name = name;
  s.callbacks.push_back(std::move(cb));
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
