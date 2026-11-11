#include "game/model_swap.h"

#include <windows.h>

#include <MinHook.h>

#include <cmath>

#include "core/log.h"

namespace ivc::model_swap {
namespace {
namespace n = natives;

constexpr DWORD kQuietMs = 400; // the player must not have jumped for this long before a swap, nor be moved after one
constexpr float kJumpM = 1.5f;  // moved this far in one frame: teleported

std::uint32_t g_pending = 0; // a swap to this model is waiting for the player to settle
DWORD g_swapped_at = 0;
DWORD g_jumped_at = 0;
n::Vector3 g_last_pos{};
bool g_have_pos = false;

// The ped's add-to-draw-list (thiscall, CPed*, 4 args, returns 0): with draw state CPed+0x314 == 0 it builds the ped's
// command (0xC645A0, which copies the render data at CPed+0x310 unless 0xC66160 says to use its other, 0x3B0-byte
// command) and then a second one (0xC64740).
constexpr std::uintptr_t kFnAddPedToDrawList = 0xC64520, kFnUsesOtherCommand = 0xC66160;
constexpr std::uintptr_t kOffRenderData = 0x310, kOffDrawState = 0x314;

template <class T> T at_static(std::uintptr_t va) {
  return reinterpret_cast<T>(va - 0x400000 + reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}

using AddPedFn = int(__thiscall*)(void*, void*, void*, void*, void*);
AddPedFn g_add_ped_orig = nullptr;
volatile LONG g_unbuilt = 0;

// Main thread, while the frame's draw list is built.
int __fastcall add_ped_detour(void* ped, void* /*edx*/, void* a, void* b, void* c, void* d) {
  if (!ped) return 0;
  const auto field = [ped](std::uintptr_t off) { return *reinterpret_cast<std::uintptr_t*>(static_cast<char*>(ped) + off); };
  if (field(kOffDrawState) == 0 && !field(kOffRenderData) && !at_static<bool(__thiscall*)(void*)>(kFnUsesOtherCommand)(ped)) {
    InterlockedIncrement(&g_unbuilt); // its command would copy from null; drawn from the next frame
    return 0;
  }
  return g_add_ped_orig(ped, a, b, c, d);
}

// The render thread's "call object->vfunc 0x1C(param)" draw command (VA 0x8DDE30, vtable 0xE81D80, queued each frame
// by 0x8F82E0 from the list at 0x19F2488 that 0x6A1A60 fills, e.g. for rage::crCreatureComponentBlendShapes): one
// naming a freed object has a stale vtable (seen: 0x437F0000, a float). Skip such a call.
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

void hook(std::uintptr_t va, void* detour, void** orig, const char* what) {
  void* target = at_static<void*>(va);
  if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK)
    log::error("Model swap: could not guard {}", what);
}

} // namespace

void install() {
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  g_image_lo = base, g_image_hi = base + nt->OptionalHeader.SizeOfImage;
  hook(kFnRenderCallbackDc, reinterpret_cast<void*>(&render_callback_detour), reinterpret_cast<void**>(&g_dc_orig),
       "the render callbacks");
  hook(kFnAddPedToDrawList, reinterpret_cast<void*>(&add_ped_detour), reinterpret_cast<void**>(&g_add_ped_orig),
       "the ped draw list");
}

bool change_player_model(n::Player player, std::uint32_t model) {
  const n::Ped ped = n::GetPlayerChar(player);
  if (!ped || !n::DoesCharExist(ped)) return false;
  g_pending = model;
  if (GetTickCount() - g_jumped_at < kQuietMs) return false; // let a teleport settle first
  // The player stays visible: hiding it first left TBoGT's facial blend shapes of the old ped registered (see the header).
  n::ChangePlayerModel(player, model);
  g_pending = 0, g_swapped_at = GetTickCount();
  return true;
}

bool busy() { return g_pending || (g_swapped_at && GetTickCount() - g_swapped_at < kQuietMs); }

void tick() {
  if (const n::Ped me = n::GetPlayerChar(n::GetPlayerId()); me && n::DoesCharExist(me)) {
    const n::Vector3 p = n::GetCharCoordinates(me);
    if (g_have_pos && std::fabs(p.x - g_last_pos.x) + std::fabs(p.y - g_last_pos.y) + std::fabs(p.z - g_last_pos.z) > kJumpM)
      g_jumped_at = GetTickCount();
    g_last_pos = p, g_have_pos = true;
  }
  static DWORD s_logged = 0;
  if ((g_stale || g_unbuilt) && GetTickCount() - s_logged > 5000) {
    s_logged = GetTickCount();
    log::warn("Model swap: skipped {} stale render callback(s) and {} unbuilt ped draw(s)", InterlockedExchange(&g_stale, 0),
              InterlockedExchange(&g_unbuilt, 0));
  }
}

} // namespace ivc::model_swap
