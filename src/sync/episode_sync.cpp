#include "sync/episode_sync.h"

#include <format>

#include "core/log.h"
#include "game/phone.h"
#include "net/bytes.h"

namespace ivc::episode_sync {
namespace {

enum Op : std::uint8_t { OpHost = 0, OpJoiner = 1 };

episode::Id g_host = episode::Id::Unknown;
std::optional<std::string> g_refusal;
unsigned g_refusals = 0;

void send(net::Session& net, Op op) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(static_cast<std::uint8_t>(op))
      .put(static_cast<std::uint8_t>(episode::current())).put(episode::protagonist_model());
  net.send_game(std::move(w.data()), true);
}

} // namespace

void host_on_player_joined(net::Session& net) { send(net, OpHost); }

void on_connected(net::Session& net) { send(net, OpJoiner); }

void on_message(const net::GameMessage& m, bool hosting) {
  net::Reader r(m.data);
  std::uint8_t type, sender, op, ep;
  std::uint32_t model;
  if (!r.get(type) || !r.get(sender) || !r.get(op) || !r.get(ep) || !r.get(model)) return;
  const episode::Id theirs = episode::from_wire(ep), mine = episode::current();
  if (op == OpJoiner) {
    if (!hosting || theirs == episode::Id::Unknown || mine == episode::Id::Unknown || theirs == mine) return;
    log::warn("Episode: player #{} is in {} but this session is {}; they will be told to switch", m.from,
              episode::name(theirs), episode::name(mine));
    phone::notify(std::format("A player in {} tried to join. This co-op game is {}.", episode::name(theirs),
                              episode::name(mine)));
    return;
  }
  if (op != OpHost || hosting || m.from != net::kHostId) return;
  g_host = theirs;
  if (theirs == episode::Id::Unknown || mine == episode::Id::Unknown) {
    log::warn("Episode: can't compare (host {}, we are {}); staying", episode::name(theirs), episode::name(mine));
    return;
  }
  if (theirs == mine) {
    log::info("Episode: the host is playing {} too", episode::name(theirs));
    return;
  }
  log::warn("Episode: the host is playing {} (protagonist {}) but we are in {}; leaving", episode::name(theirs),
            episode::protagonist_name(theirs), episode::name(mine));
  ++g_refusals;
  g_refusal = std::format("Can't join: the host is playing {}. Start {} (Episodes from Liberty City) and join again.",
                          episode::name(theirs), episode::name(theirs));
}

std::optional<std::string> take_refusal() { return std::exchange(g_refusal, std::nullopt); }

episode::Id host_episode() { return g_host; }

unsigned refusals() { return g_refusals; }

void clear() {
  g_host = episode::Id::Unknown;
  g_refusal.reset();
}

} // namespace ivc::episode_sync
