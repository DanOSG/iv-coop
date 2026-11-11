#include "game/model_swap.h"

#include <windows.h>

#include <MinHook.h>

#include <cmath>
#include <vector>

#include "core/log.h"
#include "game/player_slots.h"

namespace ivc::model_swap {
namespace {
namespace n = natives;

constexpr DWORD kHideMs = 60, kShowMs = 60; // the player is hidden at least this long each side of a swap
constexpr DWORD kQuietMs = 400;              // the player must not have jumped for this long before a swap
constexpr float kJumpM = 1.5f;               // moved this far in one frame: teleported
// Player peds are kept out of the draw list this many frames before a model change (the render thread draws the
// previous frame's list while we change it) and after the last request.
constexpr unsigned kHoldFrames = 4;

std::uint32_t g_pending = 0; // the model we hid the player for
DWORD g_hidden_at = 0, g_swapped_at = 0;
bool g_reveal = false;
DWORD g_jumped_at = 0;
n::Vector3 g_last_pos{};
bool g_have_pos = false;

unsigned g_frame = 0;
unsigned g_hold_since = 0, g_hold_last = 0; // frames: the hold's first and latest request
bool g_holding = false;
std::vector<n::Ped> g_hidden_slots; // player peds we hid for the hold (shown again when it ends)
void* volatile g_me_obj[2] = {}; // the local player's CPed* now, and the one a swap just replaced

// The ped's add-to-draw-list (thiscall, CPed*, 4 args, returns 0): with draw state CPed+0x314 == 0 it builds the ped's
// command (0xC645A0, which copies the render data at CPed+0x310 unless 0xC66160 says to use its other, 0x3B0-byte
// command) and then a second one (0xC64740).
constexpr std::uintptr_t kFnAddPedToDrawList = 0xC64520, kFnUsesOtherCommand = 0xC66160;
constexpr std::uintptr_t kOffRenderData = 0x310, kOffDrawState = 0x314;

template <class T> T at_static(std::uintptr_t va) {
  return reinterpret_cast<T>(va - 0x400000 + reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}

void* ped_object(n::Ped handle) {
  constexpr std::uintptr_t kPedPool = 0x18B6F1C, kFnPoolAt = 0x499D20; // script handle -> CPed*
  return handle ? at_static<void*(__thiscall*)(void*, n::Ped)>(kFnPoolAt)(*at_static<void**>(kPedPool), handle) : nullptr;
}

using AddPedFn = int(__thiscall*)(void*, void*, void*, void*, void*);
AddPedFn g_add_ped_orig = nullptr;
volatile LONG g_held = 0, g_unbuilt = 0;

// Main thread, while the frame's draw list is built (after the scripts, so after our tick).
int __fastcall add_ped_detour(void* ped, void* /*edx*/, void* a, void* b, void* c, void* d) {
  if (!ped) return 0;
  if (g_holding && (ped == g_me_obj[0] || ped == g_me_obj[1] || player_slots::is_slot_object(ped))) {
    InterlockedIncrement(&g_held);
    return 0;
  }
  const auto field = [ped](std::uintptr_t off) { return *reinterpret_cast<std::uintptr_t*>(static_cast<char*>(ped) + off); };
  if (field(kOffDrawState) == 0 && !field(kOffRenderData) && !at_static<bool(__thiscall*)(void*)>(kFnUsesOtherCommand)(ped)) {
    InterlockedIncrement(&g_unbuilt); // its command would copy from null
    return 0;
  }
  return g_add_ped_orig(ped, a, b, c, d);
}

// The render thread's "call object->vfunc 0x1C(param)" draw command (CPU VA 0x8DDE30, vtable 0xE81D80, queued each
// frame by 0x8F82E0 from the list at 0x19F2488 that 0x6A1A60 fills): an object freed by a player-model change while the
// previous frame's list still names it has a stale vtable (seen: 0x437F0000, a float). Skip such a call.
constexpr std::uintptr_t kFnRenderCallbackDc = 0x8DDE30;
using DcFn = void(__thiscall*)(void*);
DcFn g_dc_orig = nullptr;
volatile LONG g_stale = 0;
std::uintptr_t g_image_lo = 0, g_image_hi = 0;

bool in_image(std::uintptr_t p) { return p >= g_image_lo && p < g_image_hi; }

void __fastcall render_callback_detour(void* dc, void* /*edx*/) {
  __try {
    const auto obj = *reinterpret_cast<std::uintptr_t*>(static_cast<char*>(dc) + 8);
    const auto vt = obj ? *reinterpret_cast<std::uintptr_t*>(obj) : 0;
    if (!in_image(vt) || !in_image(*reinterpret_cast<std::uintptr_t*>(vt + 0x1C))) {
      InterlockedIncrement(&g_stale);
      return;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedIncrement(&g_stale);
    return;
  }
  g_dc_orig(dc);
}

} // namespace

void install() {
  {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    g_image_lo = base, g_image_hi = base + nt->OptionalHeader.SizeOfImage;
    void* dc = at_static<void*>(kFnRenderCallbackDc);
    if (MH_CreateHook(dc, reinterpret_cast<void*>(&render_callback_detour), reinterpret_cast<void**>(&g_dc_orig)) != MH_OK ||
        MH_EnableHook(dc) != MH_OK)
      log::error("Model swap: could not guard the render callbacks");
  }
  void* target = at_static<void*>(kFnAddPedToDrawList);
  if (MH_CreateHook(target, reinterpret_cast<void*>(&add_ped_detour), reinterpret_cast<void**>(&g_add_ped_orig)) != MH_OK ||
      MH_EnableHook(target) != MH_OK)
    log::error("Model swap: could not guard the ped draw list");
}

bool hold_player_peds() {
  if (!g_holding) {
    g_holding = true, g_hold_since = g_frame;
    g_me_obj[0] = ped_object(n::GetPlayerChar(n::GetPlayerId())), g_me_obj[1] = nullptr;
    g_hidden_slots.clear();
  }
  g_hold_last = g_frame;
  // Hidden the engine's way too (the hook above isn't on every path a visible ped is drawn by): every player ped in
  // view, ours included, since a slot changing to the co-op character rebuilds ours too when we wear it. One that
  // shows up during the hold restarts the count.
  std::vector<n::Ped> peds = player_slots::peds_in_use();
  peds.push_back(n::GetPlayerChar(n::GetPlayerId()));
  for (const n::Ped p : peds)
    if (p && n::DoesCharExist(p) && n::invoke<bool>(0x0A0F19D1 /* IS_CHAR_VISIBLE */, p)) {
      n::SetCharVisible(p, false);
      g_hidden_slots.push_back(p);
      g_hold_since = g_frame;
    }
  return g_frame - g_hold_since >= kHoldFrames;
}

bool change_player_model(n::Player player, std::uint32_t model) {
  const n::Ped ped = n::GetPlayerChar(player);
  if (!ped || !n::DoesCharExist(ped)) return false;
  const DWORD now = GetTickCount();
  if (g_pending != model) { // first call for this swap: hide, then wait a few frames
    g_pending = model, g_hidden_at = now;
    n::SetCharVisible(ped, false);
  }
  const bool held = hold_player_peds();
  if (!held || now - g_hidden_at < kHideMs || now - g_jumped_at < kQuietMs) return false; // let a teleport settle first
  void* const old_obj = ped_object(ped);
  n::ChangePlayerModel(player, model);
  g_pending = 0, g_swapped_at = now, g_reveal = true;
  if (const n::Ped swapped = n::GetPlayerChar(player)) {
    g_me_obj[0] = ped_object(swapped), g_me_obj[1] = old_obj;
    n::SetCharVisible(swapped, false); // the new ped, too
  }
  return true;
}

bool g_expect = false;

bool busy() { return g_expect || g_pending || g_reveal; }
void expect_change(bool expected) { g_expect = expected; }

void tick() {
  ++g_frame;
  if (const n::Ped me = n::GetPlayerChar(n::GetPlayerId()); me && n::DoesCharExist(me)) {
    const n::Vector3 p = n::GetCharCoordinates(me);
    if (g_have_pos && std::fabs(p.x - g_last_pos.x) + std::fabs(p.y - g_last_pos.y) + std::fabs(p.z - g_last_pos.z) > kJumpM)
      g_jumped_at = GetTickCount();
    g_last_pos = p, g_have_pos = true;
  }
  if (g_holding && g_frame - g_hold_last > kHoldFrames) {
    g_holding = false;
    g_me_obj[0] = g_me_obj[1] = nullptr;
    for (const n::Ped p : g_hidden_slots)
      if (n::DoesCharExist(p)) n::SetCharVisible(p, true);
    log::debug("Model swap: {} player ped(s) hidden for the hold", g_hidden_slots.size());
    g_hidden_slots.clear();
    log::debug("Model swap: player peds back in the draw list ({} draw(s) held back, {} unbuilt ped(s) skipped)",
               InterlockedExchange(&g_held, 0), InterlockedExchange(&g_unbuilt, 0));
  }
  static DWORD s_stale_logged = 0;
  if (g_stale && GetTickCount() - s_stale_logged > 5000) {
    s_stale_logged = GetTickCount();
    log::warn("Model swap: skipped {} render callback(s) on a freed object", InterlockedExchange(&g_stale, 0));
  }
  // A swap nobody finished (the caller stopped asking, e.g. the session ended): never leave the player hidden.
  if (g_pending && GetTickCount() - g_hidden_at > 2000) {
    g_pending = 0, g_reveal = true, g_swapped_at = 0;
    log::warn("Model swap: abandoned; showing the player again");
  }
  if (!g_reveal || GetTickCount() - g_swapped_at < kShowMs) return;
  g_reveal = false;
  if (const n::Ped ped = n::GetPlayerChar(n::GetPlayerId()); ped && n::DoesCharExist(ped)) n::SetCharVisible(ped, true);
}

} // namespace ivc::model_swap
