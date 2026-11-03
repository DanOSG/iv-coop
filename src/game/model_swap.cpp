#include "game/model_swap.h"

#include <windows.h>

#include "core/log.h"

namespace ivc::model_swap {
namespace {
namespace n = natives;

constexpr DWORD kHideMs = 60, kShowMs = 60; // a few frames each side of the swap

std::uint32_t g_pending = 0; // the model we hid the player for
DWORD g_hidden_at = 0, g_swapped_at = 0;
bool g_reveal = false;

} // namespace

bool change_player_model(n::Player player, std::uint32_t model) {
  const n::Ped ped = n::GetPlayerChar(player);
  if (!ped || !n::DoesCharExist(ped)) return false;
  const DWORD now = GetTickCount();
  if (g_pending != model) { // first call for this swap: hide, then wait a few frames
    g_pending = model, g_hidden_at = now;
    n::SetCharVisible(ped, false);
    return false;
  }
  if (now - g_hidden_at < kHideMs) return false;
  n::ChangePlayerModel(player, model);
  g_pending = 0, g_swapped_at = now, g_reveal = true;
  if (const n::Ped swapped = n::GetPlayerChar(player)) n::SetCharVisible(swapped, false); // the new ped, too
  return true;
}

void tick() {
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
