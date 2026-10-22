#pragma once
// Steam inside the game process (app 12210). GTA IV CE already runs SteamAPI; we use its
// steam_api.dll through the flat API, loaded at runtime (no Steamworks SDK is linked).
// Lobbies find a session by password; legacy ISteamNetworking P2P carries the traffic
// through Valve's relays.

#include <cstdint>
#include <string>

namespace ivc::steam {

// Resolves the API and interfaces; logs what is available. Game thread, after the game
// has initialised Steam (our first tick is fine). Returns false if Steam isn't usable.
bool init();
bool available();

std::string persona_name();
std::uint64_t my_id();

} // namespace ivc::steam
