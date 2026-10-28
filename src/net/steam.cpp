#include "net/steam.h"

#include <windows.h>

#include <deque>
#include <format>

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

// Call results (SteamAPICall_t) are polled through ISteamUtils; no callback registration.
using ApiCall = std::uint64_t;
constexpr int kLobbyCreatedId = 513;   // LobbyCreated_t: EResult, (pad), uint64 lobby (pack 8)
constexpr int kLobbyMatchListId = 510; // LobbyMatchList_t: uint32 count
constexpr int kLobbyTypePublic = 2, kMaxMembers = 8, // 4 co-op + 4 invaders
               kDistanceWorldwide = 3, kCompareEqual = 0;
constexpr const char* kKeyPassword = "ivc_pw";
constexpr const char* kKeyVersion = "ivc_ver";
constexpr const char* kKeyAddress = "ivc_addr";
constexpr const char* kKeyName = "ivc_name";
constexpr const char* kKeyOpen = "ivc_open"; // "1" while the host is open to invasion (M11)
constexpr const char* kVersion = "1";

constexpr int kLobbyEnterId = 504; // LobbyEnter_t: uint64 lobby, u32 perms, bool locked, u32 response
constexpr int kP2PUnreliable = 0, kP2PChannel = 0;

struct Pending {
  enum { Create, Search, Join } what;
  ApiCall call;
  std::string password, address;
  bool open_only = false; // Search: any lobby open to invasion, whatever its password
};
std::deque<Pending> g_pending;
std::deque<LobbyResult> g_results;
std::uint64_t g_lobby = 0;

bool call_done(ApiCall call, void* out, int size, int id, bool& failed) {
  const auto completed = fn<bool(__cdecl*)(void*, ApiCall, bool*)>("SteamAPI_ISteamUtils_IsAPICallCompleted");
  const auto result = fn<bool(__cdecl*)(void*, ApiCall, void*, int, int, bool*)>("SteamAPI_ISteamUtils_GetAPICallResult");
  bool io_failed = false;
  if (!completed(g_utils, call, &io_failed)) return false;
  failed = io_failed || !result(g_utils, call, out, size, id, &io_failed) || io_failed;
  return true;
}

const char* lobby_data(std::uint64_t lobby, const char* key) {
  const auto get = fn<const char*(__cdecl*)(void*, std::uint64_t, const char*)>("SteamAPI_ISteamMatchmaking_GetLobbyData");
  const char* v = get(g_matchmaking, lobby, key);
  return v ? v : "";
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

namespace ivc::steam {

void host_lobby(const std::string& password, const std::string& address) {
  if (!g_ok) return;
  leave_lobby();
  const auto create = fn<ApiCall(__cdecl*)(void*, int, int)>("SteamAPI_ISteamMatchmaking_CreateLobby");
  g_pending.push_back({Pending::Create, create(g_matchmaking, kLobbyTypePublic, kMaxMembers), password, address});
  log::info("Steam: creating a lobby");
}

void find_lobby(const std::string& password) {
  if (!g_ok) return;
  const auto filter = fn<void(__cdecl*)(void*, const char*, const char*, int)>(
      "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
  const auto distance = fn<void(__cdecl*)(void*, int)>("SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
  const auto request = fn<ApiCall(__cdecl*)(void*)>("SteamAPI_ISteamMatchmaking_RequestLobbyList");
  filter(g_matchmaking, kKeyPassword, password.c_str(), kCompareEqual);
  filter(g_matchmaking, kKeyVersion, kVersion, kCompareEqual);
  distance(g_matchmaking, kDistanceWorldwide);
  g_pending.push_back({Pending::Search, request(g_matchmaking), password, {}});
  log::info("Steam: searching for a lobby with password {}", password);
}

void find_open_lobby() {
  if (!g_ok) return;
  const auto filter = fn<void(__cdecl*)(void*, const char*, const char*, int)>(
      "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
  const auto distance = fn<void(__cdecl*)(void*, int)>("SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
  const auto request = fn<ApiCall(__cdecl*)(void*)>("SteamAPI_ISteamMatchmaking_RequestLobbyList");
  filter(g_matchmaking, kKeyOpen, "1", kCompareEqual);
  filter(g_matchmaking, kKeyVersion, kVersion, kCompareEqual);
  distance(g_matchmaking, kDistanceWorldwide);
  Pending p{Pending::Search, request(g_matchmaking), {}, {}};
  p.open_only = true;
  g_pending.push_back(p);
  log::info("Steam: searching for a lobby open to invasion");
}

void set_lobby_open(bool open) {
  if (!g_ok || !g_lobby) return;
  const auto set = fn<bool(__cdecl*)(void*, std::uint64_t, const char*, const char*)>(
      "SteamAPI_ISteamMatchmaking_SetLobbyData");
  set(g_matchmaking, g_lobby, kKeyOpen, open ? "1" : "0");
  log::info("Steam: lobby {} is {} to invasion", g_lobby, open ? "open" : "closed");
}

void join_lobby(std::uint64_t lobby) {
  if (!g_ok) return;
  const auto join = fn<ApiCall(__cdecl*)(void*, std::uint64_t)>("SteamAPI_ISteamMatchmaking_JoinLobby");
  Pending p{Pending::Join, join(g_matchmaking, lobby), {}, {}};
  g_pending.push_back(p);
  log::info("Steam: joining lobby {}", lobby);
}

std::vector<std::uint64_t> lobby_members() {
  std::vector<std::uint64_t> out;
  if (!g_ok || !g_lobby) return out;
  const auto count = fn<int(__cdecl*)(void*, std::uint64_t)>("SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
  const auto member = fn<std::uint64_t(__cdecl*)(void*, std::uint64_t, int)>(
      "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
  const std::uint64_t me = my_id();
  const int n = count(g_matchmaking, g_lobby);
  for (int i = 0; i < n; ++i)
    if (const std::uint64_t id = member(g_matchmaking, g_lobby, i); id && id != me) out.push_back(id);
  return out;
}

bool p2p_send(std::uint64_t to, const void* data, std::uint32_t size) {
  static const auto send = fn<bool(__cdecl*)(void*, std::uint64_t, const void*, std::uint32_t, int, int)>(
      "SteamAPI_ISteamNetworking_SendP2PPacket");
  return g_ok && send(g_networking, to, data, size, kP2PUnreliable, kP2PChannel);
}

bool p2p_read(std::vector<std::uint8_t>& out, std::uint64_t& from) {
  static const auto available = fn<bool(__cdecl*)(void*, std::uint32_t*, int)>(
      "SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
  static const auto read = fn<bool(__cdecl*)(void*, void*, std::uint32_t, std::uint32_t*, std::uint64_t*, int)>(
      "SteamAPI_ISteamNetworking_ReadP2PPacket");
  std::uint32_t size = 0;
  if (!g_ok || !available(g_networking, &size, kP2PChannel)) return false;
  out.resize(size);
  std::uint32_t got = 0;
  if (!read(g_networking, out.data(), size, &got, &from, kP2PChannel)) return false;
  out.resize(got);
  return true;
}

void p2p_accept(std::uint64_t from) {
  static const auto accept = fn<bool(__cdecl*)(void*, std::uint64_t)>("SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser");
  if (g_ok) accept(g_networking, from);
}

void p2p_close(std::uint64_t with) {
  static const auto close = fn<bool(__cdecl*)(void*, std::uint64_t)>("SteamAPI_ISteamNetworking_CloseP2PSessionWithUser");
  if (g_ok) close(g_networking, with);
}

void leave_lobby() {
  if (!g_ok || !g_lobby) return;
  fn<void(__cdecl*)(void*, std::uint64_t)>("SteamAPI_ISteamMatchmaking_LeaveLobby")(g_matchmaking, g_lobby);
  log::info("Steam: left lobby {}", g_lobby);
  g_lobby = 0;
}

void tick() {
  if (!g_ok) return;
  for (auto it = g_pending.begin(); it != g_pending.end();) {
    bool failed = false;
    if (it->what == Pending::Create) {
      struct { std::int32_t result; std::uint32_t pad; std::uint64_t lobby; } created{};
      if (!call_done(it->call, &created, sizeof(created), kLobbyCreatedId, failed)) {
        ++it;
        continue;
      }
      if (failed || created.result != 1 /* k_EResultOK */ || !created.lobby) {
        log::error("Steam: lobby creation failed (result {})", created.result);
        g_results.push_back({LobbyResult::CreateFailed});
      } else {
        g_lobby = created.lobby;
        const auto set = fn<bool(__cdecl*)(void*, std::uint64_t, const char*, const char*)>(
            "SteamAPI_ISteamMatchmaking_SetLobbyData");
        set(g_matchmaking, g_lobby, kKeyPassword, it->password.c_str());
        set(g_matchmaking, g_lobby, kKeyVersion, kVersion);
        set(g_matchmaking, g_lobby, kKeyAddress, it->address.c_str());
        set(g_matchmaking, g_lobby, kKeyName, persona_name().c_str());
        set(g_matchmaking, g_lobby, kKeyOpen, "0");
        log::info("Steam: lobby {} created (password {}, address {})", g_lobby, it->password, it->address);
        g_results.push_back({LobbyResult::Created, g_lobby});
      }
    } else if (it->what == Pending::Join) {
      struct { std::uint64_t lobby; std::uint32_t permissions; bool locked; std::uint32_t response; } entered{};
      if (!call_done(it->call, &entered, sizeof(entered), kLobbyEnterId, failed)) {
        ++it;
        continue;
      }
      if (failed || entered.response != 1 /* k_EChatRoomEnterResponseSuccess */) {
        log::error("Steam: joining lobby failed (response {})", entered.response);
        g_results.push_back({LobbyResult::JoinFailed});
      } else {
        g_lobby = entered.lobby;
        const auto owner = fn<std::uint64_t(__cdecl*)(void*, std::uint64_t)>("SteamAPI_ISteamMatchmaking_GetLobbyOwner");
        LobbyResult r{LobbyResult::Joined, g_lobby};
        r.owner = owner(g_matchmaking, g_lobby);
        log::info("Steam: in lobby {} (host {})", g_lobby, r.owner);
        g_results.push_back(r);
      }
    } else {
      std::uint32_t count = 0;
      if (!call_done(it->call, &count, sizeof(count), kLobbyMatchListId, failed)) {
        ++it;
        continue;
      }
      const auto by_index = fn<std::uint64_t(__cdecl*)(void*, int)>("SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
      LobbyResult r{LobbyResult::NotFound};
      for (std::uint32_t i = 0; !failed && i < count; ++i) {
        const std::uint64_t lobby = by_index(g_matchmaking, static_cast<int>(i));
        if (it->open_only) {
          if (std::string(lobby_data(lobby, kKeyOpen)) != "1") continue; // the filter should guarantee it
        } else if (it->password != lobby_data(lobby, kKeyPassword)) {
          continue; // the filter should guarantee it
        }
        r = {LobbyResult::Found, lobby, lobby_data(lobby, kKeyAddress), lobby_data(lobby, kKeyName)};
        break;
      }
      log::info("Steam: search for {}: {} lobbies, {}", it->open_only ? "an open invasion" : it->password, count,
                r.kind == LobbyResult::Found ? std::format("found {} ({} at {})", r.lobby, r.host_name, r.address)
                                             : std::string("no match"));
      g_results.push_back(r);
    }
    it = g_pending.erase(it);
  }
}

std::optional<LobbyResult> poll() {
  if (g_results.empty()) return std::nullopt;
  LobbyResult r = g_results.front();
  g_results.pop_front();
  return r;
}

} // namespace ivc::steam
