#pragma once
// Steam inside the game process (app 12210). GTA IV CE already runs SteamAPI; we use its
// steam_api.dll through the flat API, loaded at runtime (no Steamworks SDK is linked).
// Lobbies find a session by password; legacy ISteamNetworking P2P carries the traffic
// through Valve's relays.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ivc::steam {

// Resolves the API and interfaces; logs what is available. Game thread, after the game
// has initialised Steam (our first tick is fine). Returns false if Steam isn't usable.
bool init();
bool available();

std::string persona_name();
std::uint64_t my_id();

// --- Lobbies: a session is found by its password. All game thread; results via poll().
// Host: create a public lobby (it is only findable by its password) carrying the password,
// the host's address and name.
void host_lobby(const std::string& password, const std::string& address);
// Join: search worldwide for a lobby with this password.
void find_lobby(const std::string& password);
void leave_lobby();

// Join: enter a found lobby (so its host accepts our P2P traffic); Joined carries the owner.
void join_lobby(std::uint64_t lobby);
// Lobby we're in: its members other than us (the host accepts P2P from them).
std::vector<std::uint64_t> lobby_members();

struct LobbyResult {
  enum Kind { Created, CreateFailed, Found, NotFound, Joined, JoinFailed } kind;
  std::uint64_t lobby = 0;
  std::string address;   // host's address (Found)
  std::string host_name; // host's Steam name (Found)
  std::uint64_t owner = 0; // lobby owner's SteamID (Joined)
};
// --- P2P (legacy ISteamNetworking: relayed by Valve when a direct path isn't possible).
bool p2p_send(std::uint64_t to, const void* data, std::uint32_t size); // unreliable
// Next waiting packet, if any: fills `out` and `from`.
bool p2p_read(std::vector<std::uint8_t>& out, std::uint64_t& from);
void p2p_accept(std::uint64_t from);
void p2p_close(std::uint64_t with);

// Every frame: advances pending Steam calls.
void tick();
std::optional<LobbyResult> poll();

} // namespace ivc::steam
