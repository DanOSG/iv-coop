#include "net/steam_relay.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <map>
#include <vector>

#include "core/log.h"
#include "net/steam.h"

namespace ivc::steam_relay {
namespace {

enum class Mode { Off, Host, Join };
Mode g_mode = Mode::Off;
std::uint16_t g_enet_port = 0;     // host: the local ENet host
std::uint64_t g_host_id = 0;       // joiner: the host's SteamID
SOCKET g_join_socket = INVALID_SOCKET;
sockaddr_in g_enet_addr{};         // joiner: where our ENet sends from (learned)
bool g_have_enet_addr = false;
std::map<std::uint64_t, SOCKET> g_member_sockets; // host: one local socket per member
std::uint32_t g_stats_in = 0, g_stats_out = 0;

SOCKET local_socket() {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return s;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  u_long nonblocking = 1;
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ioctlsocket(s, FIONBIO, &nonblocking) != 0) {
    closesocket(s);
    return INVALID_SOCKET;
  }
  return s;
}

std::uint16_t port_of(SOCKET s) {
  sockaddr_in a{};
  int len = sizeof(a);
  getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
  return ntohs(a.sin_port);
}

sockaddr_in loopback(std::uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  return a;
}

// Local socket -> Steam, for every datagram waiting.
void drain_to_steam(SOCKET s, std::uint64_t to, sockaddr_in* learn_from) {
  char buf[2048];
  for (;;) {
    sockaddr_in from{};
    int from_len = sizeof(from);
    const int n = recvfrom(s, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n <= 0) return;
    if (learn_from) *learn_from = from, g_have_enet_addr = true;
    if (steam::p2p_send(to, buf, static_cast<std::uint32_t>(n))) ++g_stats_out;
  }
}

SOCKET member_socket(std::uint64_t id) {
  if (auto it = g_member_sockets.find(id); it != g_member_sockets.end()) return it->second;
  const SOCKET s = local_socket();
  if (s == INVALID_SOCKET) return s;
  g_member_sockets[id] = s;
  log::info("Relay: member {} on local port {}", id, port_of(s));
  return s;
}

} // namespace

bool start_host(std::uint16_t enet_port) {
  stop();
  if (!steam::available()) return false;
  g_mode = Mode::Host;
  g_enet_port = enet_port;
  log::info("Relay: hosting over Steam P2P (ENet port {})", enet_port);
  return true;
}

std::optional<std::uint16_t> start_join(std::uint64_t host_id) {
  stop();
  if (!steam::available() || !host_id) return std::nullopt;
  g_join_socket = local_socket();
  if (g_join_socket == INVALID_SOCKET) {
    log::error("Relay: no local socket");
    return std::nullopt;
  }
  g_mode = Mode::Join;
  g_host_id = host_id;
  steam::p2p_accept(host_id);
  const std::uint16_t port = port_of(g_join_socket);
  log::info("Relay: joining {} over Steam P2P via local port {}", host_id, port);
  return port;
}

void stop() {
  if (g_mode == Mode::Off) return;
  if (g_join_socket != INVALID_SOCKET) closesocket(g_join_socket);
  g_join_socket = INVALID_SOCKET;
  for (auto& [id, s] : g_member_sockets) {
    closesocket(s);
    steam::p2p_close(id);
  }
  g_member_sockets.clear();
  if (g_host_id) steam::p2p_close(g_host_id);
  log::info("Relay: stopped ({} packets in, {} out)", g_stats_in, g_stats_out);
  g_mode = Mode::Off, g_host_id = 0, g_have_enet_addr = false, g_stats_in = g_stats_out = 0;
}

bool active() { return g_mode != Mode::Off; }

void tick() {
  if (g_mode == Mode::Off) return;
  if (g_mode == Mode::Host) {
    // Accept P2P from lobby members (no callbacks needed); give each a local socket.
    for (std::uint64_t id : steam::lobby_members()) {
      steam::p2p_accept(id);
      member_socket(id);
    }
    for (auto& [id, s] : g_member_sockets) drain_to_steam(s, id, nullptr); // ENet replies -> member
  } else {
    drain_to_steam(g_join_socket, g_host_id, &g_enet_addr); // our ENet -> host
  }
  // Steam -> local.
  std::vector<std::uint8_t> packet;
  std::uint64_t from = 0;
  while (steam::p2p_read(packet, from)) {
    ++g_stats_in;
    if (g_mode == Mode::Host) {
      const auto it = g_member_sockets.find(from); // only members of our lobby
      if (it == g_member_sockets.end()) continue;
      const SOCKET s = it->second;
      const sockaddr_in to = loopback(g_enet_port);
      sendto(s, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
             reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    } else if (from == g_host_id && g_have_enet_addr) {
      sendto(g_join_socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
             reinterpret_cast<const sockaddr*>(&g_enet_addr), sizeof(g_enet_addr));
    }
  }
}

} // namespace ivc::steam_relay
