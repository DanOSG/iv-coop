#pragma once
// Carries the session's ENet traffic over Steam P2P (Valve relays it when a direct path
// isn't possible), so joining by password works without port forwarding. ENet itself is
// untouched: it talks to local UDP sockets, and this proxy moves those datagrams to and
// from SteamIDs.
//   host:   one local socket per remote member; their packets are sent from it to the
//           host's ENet port, and its replies go back to that member over Steam.
//   joiner: ENet connects to a local proxy port; everything goes to the host's SteamID.
// Runs from the game tick (the thread the game uses Steam from).

#include <cstdint>
#include <optional>

namespace ivc::steam_relay {

// Host: relay members of our Steam lobby to the local ENet host on `enet_port`.
bool start_host(std::uint16_t enet_port);
// Joiner: relay to `host_id`; returns the local port ENet should connect to.
std::optional<std::uint16_t> start_join(std::uint64_t host_id);
void stop();
bool active();
// Every frame.
void tick();

} // namespace ivc::steam_relay
