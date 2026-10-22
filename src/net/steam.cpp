#include "net/steam.h"

#include <windows.h>

#include "core/log.h"

namespace ivc::steam {
namespace {

using HSteamUser = std::int32_t;
using GetUserFn = HSteamUser(__cdecl*)();
using FindIfaceFn = void*(__cdecl*)(HSteamUser, const char*);
using PersonaFn = const char*(__cdecl*)(void*);
using SteamIdFn = std::uint64_t(__cdecl*)(void*);

HMODULE g_dll = nullptr;
bool g_ok = false;
void* g_user = nullptr;
void* g_friends = nullptr;
void* g_matchmaking = nullptr;
void* g_networking = nullptr;
void* g_utils = nullptr;

template <class F> F fn(const char* name) { return reinterpret_cast<F>(GetProcAddress(g_dll, name)); }

// Tries interface versions newest-first; the Steam client serves any version it knows.
void* iface(FindIfaceFn find, HSteamUser user, std::initializer_list<const char*> versions, const char* what) {
  for (const char* v : versions)
    if (void* p = find(user, v)) {
      log::info("Steam: {} = {} ({})", what, p, v);
      return p;
    }
  log::warn("Steam: no {} interface", what);
  return nullptr;
}

} // namespace

bool init() {
  if (g_ok) return true;
  g_dll = GetModuleHandleW(L"steam_api.dll");
  if (!g_dll) {
    log::warn("Steam: steam_api.dll not loaded (not a Steam copy?)");
    return false;
  }
  const auto get_user = fn<GetUserFn>("SteamAPI_GetHSteamUser");
  const auto find = fn<FindIfaceFn>("SteamInternal_FindOrCreateUserInterface");
  if (!get_user || !find) {
    log::warn("Steam: flat API entry points missing");
    return false;
  }
  const HSteamUser user = get_user();
  if (!user) {
    log::warn("Steam: no Steam user (Steam not running, or the game didn't initialise it)");
    return false;
  }
  g_user = iface(find, user, {"SteamUser021", "SteamUser020", "SteamUser019"}, "user");
  g_friends = iface(find, user, {"SteamFriends017", "SteamFriends016", "SteamFriends015"}, "friends");
  g_matchmaking = iface(find, user, {"SteamMatchMaking009", "SteamMatchMaking008"}, "matchmaking");
  g_networking = iface(find, user, {"SteamNetworking006", "SteamNetworking005"}, "networking (P2P)");
  g_utils = iface(find, user, {"SteamUtils010", "SteamUtils009"}, "utils");
  g_ok = g_user && g_friends && g_matchmaking && g_networking && g_utils;
  if (g_ok) log::info("Steam: ready as '{}' ({})", persona_name(), my_id());
  return g_ok;
}

bool available() { return g_ok; }

std::string persona_name() {
  const auto f = fn<PersonaFn>("SteamAPI_ISteamFriends_GetPersonaName");
  const char* s = (f && g_friends) ? f(g_friends) : nullptr;
  return s ? s : "";
}

std::uint64_t my_id() {
  const auto f = fn<SteamIdFn>("SteamAPI_ISteamUser_GetSteamID");
  return (f && g_user) ? f(g_user) : 0;
}

} // namespace ivc::steam
