#include "game/episode.h"

#include <windows.h>

#include <algorithm>

#include "core/log.h"
#include "game/natives.h"
#include "game/radio.h"

namespace ivc::episode {
namespace {
namespace n = natives;

constexpr std::uint32_t kGetCurrentEpisode = 0x7D7619D2;
constexpr std::uint32_t kPlayerModel = 0x6F0783F5; // joaat("player")
constexpr DWORD kRecheckMs = 2000;

Id g_current = Id::Unknown;
DWORD g_checked = 0;
std::uint32_t g_measured = 0;     // the local player's model as first reported in this episode
bool g_measured_logged = false;

} // namespace

Id from_wire(std::uint8_t v) { return v <= 2 ? static_cast<Id>(v) : Id::Unknown; }

Id current() { return g_current; }

const char* name(Id id) {
  switch (id) {
    case Id::IV: return "Grand Theft Auto IV";
    case Id::TLAD: return "The Lost and Damned";
    case Id::TBoGT: return "The Ballad of Gay Tony";
    default: return "an unknown episode";
  }
}

const char* short_name(Id id) {
  switch (id) {
    case Id::IV: return "GTA IV";
    case Id::TLAD: return "TLAD";
    case Id::TBoGT: return "TBoGT";
    default: return "?";
  }
}

const char* protagonist_name(Id id) {
  switch (id) {
    case Id::IV: return "Niko";
    case Id::TLAD: return "Johnny";
    case Id::TBoGT: return "Luis";
    default: return "the protagonist";
  }
}

void tick() {
  const DWORD now = GetTickCount();
  if (g_current != Id::Unknown && now - g_checked < kRecheckMs) return;
  g_checked = now;
  const Id seen = from_wire(static_cast<std::uint8_t>(std::min<std::uint32_t>(n::invoke<std::uint32_t>(kGetCurrentEpisode), 0xFF)));
  if (seen == g_current) return;
  if (seen == Id::Unknown) {
    log::warn("Episode: GET_CURRENT_EPISODE returned an unknown value; treating the game as unidentified");
  } else {
    log::info("Episode: {} ({}); protagonist {}; radio stations {}", name(seen), short_name(seen), protagonist_name(seen),
              radio::station_count());
  }
  if (g_current != Id::Unknown) {
    g_measured = 0, g_measured_logged = false; // another episode loaded: measure again
  }
  g_current = seen;
}

namespace engine {
template <class T> T at_static(std::uintptr_t va) {
  return reinterpret_cast<T>(va - 0x400000 + reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}
using SetInt = std::uint32_t(__cdecl*)(std::int32_t);
constexpr std::uintptr_t kMenuEpisode = 0x1160C08; // the episode the frontend's menu picked
constexpr std::int32_t kStartEpisode = 0x3E;
} // namespace engine

void request_switch(Id id) {
  if (id == Id::Unknown) return;
  *engine::at_static<volatile std::int32_t*>(engine::kMenuEpisode) = static_cast<std::int32_t>(id);
  engine::at_static<engine::SetInt>(0x8BECC0)(1);                    // frontend mode ([0x1030B7C])
  engine::at_static<engine::SetInt>(0x8BE0E0)(engine::kStartEpisode); // pending action ([0x1160C24])
  log::warn("Episode: starting {} (frontend action 0x3E)", name(id));
}

std::uint32_t protagonist_model() { return g_measured ? g_measured : kPlayerModel; }

void note_player_model(std::uint32_t model) {
  if (g_measured || !model || g_current == Id::Unknown || g_current == Id::IV) return; // IV: the constant, as ever
  if (model == kPlayerModel) { // the expected one: nothing to override
    g_measured = kPlayerModel;
    return;
  }
  // Another model: only trust it if it is a real ped model of the image.
  if (!n::IsModelInCdimage(model) || !n::IsThisModelAPed(model)) return;
  g_measured = model;
  if (!g_measured_logged) {
    g_measured_logged = true;
    log::warn("Episode: the {} protagonist wears model 0x{:08X}, not the generic \"player\" (0x{:08X}); using it for "
              "puppets and cutscenes",
              protagonist_name(g_current), model, kPlayerModel);
  }
}

} // namespace ivc::episode
