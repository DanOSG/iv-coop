#include "net/session.h"

#include <cstring>

#include <enet/enet.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>
#include <cstdio>
#include <format>
#include <random>

namespace ivc::net {
namespace {

std::once_flag g_enet_once;
bool g_enet_ok = false;

std::string reason_text(std::uint32_t data) {
  switch (reject_reason(data)) {
    case Reject::Full: return "session is full";
    case Reject::VersionMismatch: {
      const std::uint16_t host_v = reject_host_version(data);
      if (!host_v) return "version mismatch: update iv-coop (host and joiner must run the same version)";
      return std::format("version mismatch: the host runs protocol v{}, you run v{}; "
                         "update iv-coop so you both have the same version",
                         host_v, kProtocolVersion);
    }
    case Reject::HostLeft: return "host closed the session";
    case Reject::Kicked: return "kicked by the host (too much traffic)";
    case Reject::HelloTimeout: return "handshake timed out";
    case Reject::NotOpen: return "that session isn't open to invasion";
    default: return "connection lost";
  }
}

const char* type_name(std::uint8_t t) {
  switch (static_cast<MsgType>(t)) {
    case MsgType::Hello: return "Hello";
    case MsgType::Welcome: return "Welcome";
    case MsgType::PlayerJoined: return "Joined";
    case MsgType::PlayerLeft: return "Left";
    case MsgType::PlayerState: return "PlayerState";
    case MsgType::VehicleState: return "VehicleState";
    default: return nullptr;
  }
}

struct Traffic {
  std::uint64_t bytes_in = 0, bytes_out = 0;
  std::uint32_t pkts_in = 0, pkts_out = 0;
  std::array<std::uint32_t, 256> in{}, out{}; // packets per message type
};

std::string top5(const std::array<std::uint32_t, 256>& counts) {
  std::vector<std::pair<std::uint32_t, int>> v;
  for (int t = 0; t < 256; ++t)
    if (counts[t]) v.push_back({counts[t], t});
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::string out;
  for (std::size_t i = 0; i < v.size() && i < 5; ++i) {
    const char* n = type_name(static_cast<std::uint8_t>(v[i].second));
    out += n ? std::format(" {}x{}", n, v[i].first) : std::format(" #{}x{}", v[i].second, v[i].first);
  }
  return out.empty() ? " -" : out;
}

void configure_peer(ENetPeer* peer) {
  // A peer that stops answering (crash, killed process, cable pulled) is dropped after at
  // most 8 s, so it can't hold a session slot for ENet's default 30 s.
  enet_peer_timeout(peer, 0, 3000, 8000);
  // ENet throttles unreliable packets when RTT fluctuates; on fast links tiny jitter reads
  // as congestion and it silently drops 20-30% of our state packets (seen as ~0.5-2 s gaps).
  // Our traffic is a few KB/s, so never back off: deceleration 0 keeps the throttle at max.
  enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_ACCELERATION, 0);
}

void send_packet(ENetPeer* peer, std::uint8_t channel, std::span<const std::uint8_t> data, bool reliable) {
  ENetPacket* p = enet_packet_create(data.data(), data.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
  if (enet_peer_send(peer, channel, p) != 0) enet_packet_destroy(p);
}

} // namespace

std::uint32_t steady_ms() {
  using namespace std::chrono;
  return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

const char* to_string(Mode m) {
  switch (m) {
    case Mode::Idle: return "idle";
    case Mode::Hosting: return "hosting";
    case Mode::Connecting: return "connecting";
    case Mode::Joined: return "joined";
  }
  return "?";
}

struct Session::Impl {
  struct Client {
    ENetPeer* peer;
    std::uint8_t id;
    Name name{};
    Role role = Role::Coop;
    bool greeted = false;
    std::uint32_t connected_ms = 0;
    std::uint32_t win_start_ms = 0, win_pkts = 0; // flood window (1 s)
    int over_windows = 0;                         // consecutive windows above kMaxPacketsPerSec
  };

  // Owned by whichever thread is not running the loop: the caller before start()/after
  // stop(), the network thread in between.
  ENetHost* host = nullptr;
  ENetPeer* server = nullptr;
  std::vector<Client> clients;
  std::thread thread;
  std::atomic<bool> running{false};

  // Shared with the caller; guarded by m.
  mutable std::mutex m;
  SessionStatus st;
  std::vector<RosterEntry> roster; // everyone, including us
  Name my_name{};
  Role my_role = Role::Coop;
  bool kick_invaders_pending = false;
  Reject kick_reason = Reject::NotOpen;
  std::atomic<bool> invasion_open{false};
  std::deque<std::vector<std::uint8_t>> out_states;
  std::deque<StateUpdate> in_states;
  std::deque<VehicleUpdate> in_vehicles;
  struct OutGame {
    std::vector<std::uint8_t> data;
    bool reliable;
  };
  std::deque<OutGame> out_game;
  std::deque<GameMessage> in_game;
  LogFn logger;
  Chaos chaos_cfg;
  std::uint32_t blackhole_until = 0;
  std::uint64_t total_out = 0, total_in = 0;
  std::uint32_t dropped_in = 0, dropped_out = 0;

  // Network-thread state: traffic accounting for the 10 s summary, and fault injection.
  std::array<Traffic, kMaxPlayers> traffic;
  std::uint32_t stats_t0 = 0;
  Chaos chaos;
  std::uint32_t blackhole_to = 0;
  std::mt19937 rng{std::random_device{}()};
  struct Delayed {
    std::uint32_t due;
    ENetPeer* peer;
    std::uint8_t channel;
    std::vector<std::uint8_t> data;
    bool reliable;
  };
  std::vector<Delayed> delayed;

  // Recording (guarded by m).
  std::FILE* rec = nullptr;
  std::uint32_t rec_start_ms = 0;
  std::uint32_t rec_count = 0;

  void push_game(std::uint8_t from, std::span<const std::uint8_t> data) {
    std::lock_guard lock(m);
    in_game.push_back({from, {data.begin(), data.end()}, steady_ms()});
    if (in_game.size() > 2048) in_game.pop_front(), ++dropped_in; // game thread stalled; oldest lost
  }

  void log(int level, const std::string& text) {
    LogFn fn;
    {
      std::lock_guard lock(m);
      fn = logger;
    }
    if (fn) fn(level, text);
  }

  float chance() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng); }

  // All outgoing traffic goes through here: accounting, then (test rig) fault injection.
  void tx(ENetPeer* peer, std::uint8_t pid, std::uint8_t ch, std::span<const std::uint8_t> data, bool reliable) {
    if (data.empty() || !peer) return;
    if (pid < kMaxPlayers) {
      Traffic& t = traffic[pid];
      t.bytes_out += data.size(), ++t.pkts_out, ++t.out[data[0]];
    }
    {
      std::lock_guard lock(m);
      total_out += data.size();
    }
    if (!chaos.any()) return send_packet(peer, ch, data, reliable);
    if (!reliable && chance() < chaos.drop) return;
    std::vector<std::uint8_t> d(data.begin(), data.end());
    if (d[0] >= static_cast<std::uint8_t>(MsgType::PlayerState)) { // never mangle the handshake
      if (d.size() > 2 && chance() < chaos.truncate) d.resize(1 + rng() % (d.size() - 1));
      if (d.size() > 2 && chance() < chaos.corrupt) d[2 + rng() % (d.size() - 2)] ^= static_cast<std::uint8_t>(1 + rng() % 255);
    }
    if (chance() < chaos.duplicate) delayed.push_back({steady_ms() + rng() % 60, peer, ch, d, reliable});
    if (chance() < chaos.delay) {
      delayed.push_back({steady_ms() + 10 + rng() % std::max<std::uint32_t>(chaos.max_delay_ms, 11), peer, ch, d, reliable});
      return;
    }
    send_packet(peer, ch, d, reliable);
  }

  void flush_delayed(std::uint32_t now) {
    for (std::size_t i = 0; i < delayed.size();) {
      if (static_cast<std::int32_t>(now - delayed[i].due) < 0) {
        ++i;
        continue;
      }
      Delayed d = std::move(delayed[i]);
      delayed.erase(delayed.begin() + static_cast<std::ptrdiff_t>(i));
      if (d.peer->state == ENET_PEER_STATE_CONNECTED) send_packet(d.peer, d.channel, d.data, d.reliable);
    }
  }

  void note_in(std::uint8_t pid, std::span<const std::uint8_t> data) {
    if (pid < kMaxPlayers && !data.empty()) {
      Traffic& t = traffic[pid];
      t.bytes_in += data.size(), ++t.pkts_in, ++t.in[data[0]];
    }
    std::lock_guard lock(m);
    total_in += data.size();
  }

  // Every 10 s: bytes/s and packets/s per peer, and the five busiest message types, so a
  // flood (or a chatty subsystem) shows in the log.
  void log_stats(std::uint32_t now) {
    if (now - stats_t0 < 10000) return;
    const float secs = std::max((now - stats_t0) / 1000.0f, 0.001f);
    stats_t0 = now;
    for (std::uint8_t id = 0; id < kMaxPlayers; ++id) {
      const Traffic& t = traffic[id];
      if (!t.pkts_in && !t.pkts_out) continue;
      std::string who = id == kHostId ? "host" : "?";
      std::uint32_t rtt = 0;
      for (const auto& c : clients)
        if (c.id == id) who = to_string(c.name), rtt = c.peer->roundTripTime;
      if (id == kHostId && server) rtt = server->roundTripTime;
      log(0, std::format("net 10s: peer #{} '{}' rtt {} ms | out {:.1f} KB/s {:.0f} pkt/s | in {:.1f} KB/s {:.0f} pkt/s | "
                         "top out:{} | top in:{}",
                         id, who, rtt, t.bytes_out / 1024.0f / secs, t.pkts_out / secs, t.bytes_in / 1024.0f / secs,
                         t.pkts_in / secs, top5(t.out), top5(t.in)));
    }
    traffic = {};
    std::uint32_t din, dout;
    {
      std::lock_guard lock(m);
      din = dropped_in, dout = dropped_out;
    }
    if (din || dout)
      log(1, std::format("net: {} packets dropped on receive, {} on send so far (oversize, malformed, flood, overflow)", din, dout));
  }
  std::deque<SessionEvent> events;

  void start() {
    running = true;
    thread = std::thread([this] { loop(); });
  }

  void stop() {
    running = false;
    if (thread.joinable()) thread.join();
    if (host) {
      if (server) enet_peer_disconnect_now(server, 0);
      for (auto& c : clients) enet_peer_disconnect_now(c.peer, static_cast<std::uint32_t>(Reject::HostLeft));
      enet_host_flush(host);
      enet_host_destroy(host);
    }
    host = nullptr;
    server = nullptr;
    clients.clear();
    std::lock_guard lock(m);
    roster.clear();
    out_states.clear();
    out_game.clear();
    in_states.clear(); // stale packets must not leak into the next session
    in_vehicles.clear();
    in_game.clear();
    delayed.clear();
    blackhole_to = blackhole_until = 0;
    traffic = {};
    stats_t0 = 0;
    total_in = total_out = 0;
    dropped_in = dropped_out = 0;
    st = {};
  }

  void event(SessionEvent::Kind k, std::uint8_t id, std::string text, Role role = Role::Coop) {
    std::lock_guard lock(m);
    events.push_back({k, id, std::move(text), role});
  }

  Client* client_by_peer(ENetPeer* p) {
    for (auto& c : clients)
      if (c.peer == p) return &c;
    return nullptr;
  }

  void push_state(std::uint8_t id, const sync::PlayerState& s) {
    std::lock_guard lock(m);
    in_states.push_back({id, s, steady_ms()});
    if (in_states.size() > 512) in_states.pop_front(); // caller stalled; keep it bounded
    ++st.states_received;
  }

  void push_vehicle(std::uint8_t id, const sync::VehicleState& v) {
    std::lock_guard lock(m);
    in_vehicles.push_back({id, v, steady_ms()});
    if (in_vehicles.size() > 512) in_vehicles.pop_front();
    ++st.states_received;
  }

  // Decodes a PlayerState/VehicleState packet (type byte already read) and queues it for
  // the caller as coming from `from`. Returns false if malformed.
  bool accept_state(MsgType type, Reader& r, std::uint8_t from) {
    std::uint8_t id;
    if (type == MsgType::PlayerState) {
      sync::PlayerState s;
      if (!decode_state(r, id, s)) return false;
      push_state(from, s);
      return true;
    }
    sync::VehicleState v;
    if (!decode_vehicle(r, id, v)) return false;
    push_vehicle(from, v);
    return true;
  }

  // --- host side ---
  void host_on_connect(ENetEvent& ev) {
    configure_peer(ev.peer);
    if (ev.data != kProtocolVersion) {
      log(1, std::format("rejecting a joiner: it runs protocol v{}, we run v{}", ev.data, kProtocolVersion));
      enet_peer_disconnect(ev.peer, reject_data(Reject::VersionMismatch));
      return;
    }
    for (std::uint8_t id = 1; id < kMaxPlayers; ++id) { // 1..7; the co-op/invader limits are checked at Hello
      bool used = false;
      for (const auto& c : clients) used |= c.id == id;
      if (!used) {
        clients.push_back({ev.peer, id, {}, Role::Coop, false, steady_ms()});
        return; // announced once their Hello arrives
      }
    }
    enet_peer_disconnect(ev.peer, reject_data(Reject::Full));
  }

  void host_on_receive(ENetEvent& ev) {
    Client* c = client_by_peer(ev.peer);
    if (!c) return;
    std::span<std::uint8_t> data(ev.packet->data, ev.packet->dataLength);
    if (data.empty() || data.size() > kMaxPacket) {
      std::lock_guard lock(m);
      ++dropped_in;
      return;
    }
    note_in(c->id, data);
    // Flood guard: one-second windows. Packets beyond the limit are dropped; a peer that
    // stays over it for kFloodSeconds in a row is kicked.
    const std::uint32_t now = steady_ms();
    if (now - c->win_start_ms >= 1000) {
      if (c->win_pkts > kMaxPacketsPerSec) {
        ++c->over_windows;
        log(1, std::format("flood: player #{} sent {} packets in 1 s ({} s in a row)", c->id, c->win_pkts, c->over_windows));
        if (c->over_windows >= kFloodSeconds) {
          log(1, std::format("kicking player #{} for flooding", c->id));
          enet_peer_disconnect(ev.peer, reject_data(Reject::Kicked));
          return;
        }
      } else {
        c->over_windows = 0;
      }
      c->win_start_ms = now, c->win_pkts = 0;
    }
    if (++c->win_pkts > kMaxPacketsPerSec) {
      std::lock_guard lock(m);
      ++dropped_in;
      return;
    }
    Reader r(data);
    MsgType type{};
    r.get(type);

    if (type == MsgType::Hello && !c->greeted) {
      std::uint16_t version = 0;
      r.get(version), r.get(c->name);
      Role role = Role::Coop;
      r.get(role);
      if (!r.ok() || version != kProtocolVersion || role > Role::Invader) {
        log(1, std::format("rejecting a joiner: Hello with protocol v{}, we run v{}", version, kProtocolVersion));
        enet_peer_disconnect(ev.peer, reject_data(Reject::VersionMismatch));
        return;
      }
      c->name.back() = 0; // never trust a terminator from the wire
      ModVersion mv{};
      if (r.remaining() >= sizeof(mv)) r.get(mv); // older builds don't send it
      mv.back() = 0;
      log(0, std::format("player #{} '{}' says hello (protocol v{}, mod {})", c->id, to_string(c->name), version,
                         mv[0] ? mv.data() : "?"));
      // Co-op group: host + 3. Invaders: only while the host's invasion window is open, up to 4.
      int coop = 1, invaders = 0; // the host counts as co-op
      for (const auto& o : clients)
        if (o.greeted) (o.role == Role::Invader ? invaders : coop)++;
      if (role == Role::Invader && !invasion_open) {
        enet_peer_disconnect(ev.peer, reject_data(Reject::NotOpen));
        return;
      }
      if ((role == Role::Invader && invaders >= kMaxInvaders) || (role == Role::Coop && coop >= kMaxCoop)) {
        enet_peer_disconnect(ev.peer, reject_data(Reject::Full));
        return;
      }
      c->role = role;
      c->greeted = true;
      std::vector<RosterEntry> snapshot;
      {
        std::lock_guard lock(m);
        roster.push_back({c->id, c->name, role});
        snapshot = roster;
      }
      tx(ev.peer, c->id, kChanControl, encode_welcome(c->id, snapshot), true);
      const auto joined = encode_joined(c->id, c->name, role);
      for (auto& other : clients)
        if (other.greeted && other.peer != ev.peer) tx(other.peer, other.id, kChanControl, joined, true);
      event(SessionEvent::Kind::Joined, c->id, to_string(c->name), role);
    } else if ((type == MsgType::PlayerState || type == MsgType::VehicleState) && c->greeted) {
      if (!accept_state(type, r, c->id)) return;
      restamp_state(data, c->id); // never trust the sender's id
      for (auto& other : clients)
        if (other.greeted && other.peer != ev.peer) tx(other.peer, other.id, kChanState, data, false);
    } else if (static_cast<std::uint8_t>(type) >= kFirstGameMsg && c->greeted && data.size() >= 2) {
      restamp_state(data, c->id);
      push_game(c->id, data);
      const bool reliable = (ev.packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0;
      for (auto& other : clients)
        if (other.greeted && other.peer != ev.peer)
          tx(other.peer, other.id, reliable ? kChanControl : kChanState, data, reliable);
    }
  }

  void host_on_disconnect(ENetEvent& ev) {
    for (auto it = clients.begin(); it != clients.end(); ++it) {
      if (it->peer != ev.peer) continue;
      if (it->greeted) {
        const std::uint8_t id = it->id;
        {
          std::lock_guard lock(m);
          std::erase_if(roster, [id](const RosterEntry& e) { return e.id == id; });
        }
        const auto left = encode_left(id);
        for (auto& other : clients)
          if (other.greeted && other.peer != ev.peer) tx(other.peer, other.id, kChanControl, left, true);
        event(SessionEvent::Kind::Left, id, to_string(it->name));
      }
      clients.erase(it);
      return;
    }
  }

  // --- client side ---
  void client_on_receive(ENetEvent& ev) {
    if (ev.packet->dataLength == 0 || ev.packet->dataLength > kMaxPacket) {
      std::lock_guard lock(m);
      ++dropped_in;
      return;
    }
    note_in(kHostId, std::span<const std::uint8_t>(ev.packet->data, ev.packet->dataLength));
    Reader r(std::span<const std::uint8_t>(ev.packet->data, ev.packet->dataLength));
    MsgType type{};
    r.get(type);
    if (type == MsgType::Welcome) {
      std::uint8_t my_id = 0, count = 0;
      r.get(my_id), r.get(count);
      if (count > kMaxPlayers) return;
      std::vector<RosterEntry> list(count);
      for (auto& e : list) r.get(e.id), r.get(e.name), r.get(e.role);
      if (!r.ok()) return;
      Role mine = my_role;
      for (const auto& e : list)
        if (e.id == my_id) mine = e.role;
      {
        std::lock_guard lock(m);
        roster = list;
        st.my_id = my_id;
        st.my_role = mine;
        st.mode = Mode::Joined;
      }
      event(SessionEvent::Kind::Connected, my_id, {}, mine);
      for (const auto& e : list)
        if (e.id != my_id) event(SessionEvent::Kind::Joined, e.id, to_string(e.name), e.role);
    } else if (type == MsgType::PlayerJoined) {
      RosterEntry e{};
      r.get(e.id), r.get(e.name), r.get(e.role);
      if (!r.ok()) return;
      e.name.back() = 0;
      {
        std::lock_guard lock(m);
        std::erase_if(roster, [&](const RosterEntry& o) { return o.id == e.id; });
        roster.push_back(e);
      }
      event(SessionEvent::Kind::Joined, e.id, to_string(e.name), e.role);
    } else if (type == MsgType::PlayerLeft) {
      std::uint8_t id = 0;
      if (!r.get(id)) return;
      std::string name;
      {
        std::lock_guard lock(m);
        for (const auto& e : roster)
          if (e.id == id) name = to_string(e.name);
        std::erase_if(roster, [id](const RosterEntry& e) { return e.id == id; });
      }
      event(SessionEvent::Kind::Left, id, name);
    } else if (type == MsgType::PlayerState || type == MsgType::VehicleState) {
      // The sender id is the byte right after the type; skip our own echoes.
      if (ev.packet->dataLength > 1 && ev.packet->data[1] != st.my_id) accept_state(type, r, ev.packet->data[1]);
    } else if (static_cast<std::uint8_t>(type) >= kFirstGameMsg && ev.packet->dataLength >= 2 &&
               ev.packet->data[1] != st.my_id) {
      push_game(ev.packet->data[1], std::span<const std::uint8_t>(ev.packet->data, ev.packet->dataLength));
    }
  }

  void loop() {
    while (running) {
      std::deque<std::vector<std::uint8_t>> out;
      std::deque<OutGame> game;
      Mode mode;
      bool kick = false;
      Reject kick_why = Reject::NotOpen;
      {
        std::lock_guard lock(m);
        out.swap(out_states);
        game.swap(out_game);
        mode = st.mode;
        kick = std::exchange(kick_invaders_pending, false);
        kick_why = kick_reason;
        chaos = chaos_cfg;
        if (blackhole_until) blackhole_to = blackhole_until, blackhole_until = 0;
      }
      const std::uint32_t tick_ms = steady_ms();
      // Chaos "dead link": stop servicing ENet altogether (no acks, no pings), so the peer
      // sees silence and times us out, as if the cable were pulled.
      if (blackhole_to) {
        if (static_cast<std::int32_t>(tick_ms - blackhole_to) < 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
        }
        blackhole_to = 0;
      }
      if (!stats_t0) stats_t0 = tick_ms;
      flush_delayed(tick_ms);
      log_stats(tick_ms);
      // Joiners that connected but never said Hello (a port scanner, a stuck client) must
      // not hold one of the few peer slots.
      if (mode == Mode::Hosting) {
        for (auto& c : clients)
          if (!c.greeted && tick_ms - c.connected_ms > kHelloTimeoutMs && c.peer->state == ENET_PEER_STATE_CONNECTED) {
            log(1, "dropping a connection that never sent Hello");
            enet_peer_disconnect(c.peer, reject_data(Reject::HelloTimeout));
          }
      }
      if (kick && mode == Mode::Hosting)
        for (auto& c : clients)
          if (c.role == Role::Invader) enet_peer_disconnect(c.peer, reject_data(kick_why));
      for (const auto& g : game) {
        const std::uint8_t ch = g.reliable ? kChanControl : kChanState;
        if (mode == Mode::Hosting) {
          for (auto& c : clients)
            if (c.greeted) tx(c.peer, c.id, ch, g.data, g.reliable);
        } else if (mode == Mode::Joined && server) {
          tx(server, kHostId, ch, g.data, g.reliable);
        }
      }
      for (const auto& pkt : out) {
        if (mode == Mode::Hosting) {
          for (auto& c : clients)
            if (c.greeted) tx(c.peer, c.id, kChanState, pkt, false);
        } else if (mode == Mode::Joined && server) {
          tx(server, kHostId, kChanState, pkt, false);
        }
        std::lock_guard lock(m);
        ++st.states_sent;
      }

      ENetEvent ev;
      int r = enet_host_service(host, &ev, 5);
      while (r > 0) {
        switch (ev.type) {
          case ENET_EVENT_TYPE_CONNECT:
            if (mode == Mode::Hosting) {
              host_on_connect(ev);
            } else {
              configure_peer(ev.peer);
              tx(ev.peer, kHostId, kChanControl, encode_hello(to_string(my_name), my_role), true);
            }
            break;
          case ENET_EVENT_TYPE_RECEIVE:
            if (mode == Mode::Hosting)
              host_on_receive(ev);
            else
              client_on_receive(ev);
            enet_packet_destroy(ev.packet);
            break;
          case ENET_EVENT_TYPE_DISCONNECT:
            if (mode == Mode::Hosting) {
              host_on_disconnect(ev);
            } else {
              const std::string why = reason_text(ev.data);
              {
                std::lock_guard lock(m);
                st.mode = Mode::Idle;
                st.last_error = why;
                roster.clear();
              }
              server = nullptr;
              event(SessionEvent::Kind::Disconnected, 0, why);
            }
            break;
          default:
            break;
        }
        {
          std::lock_guard lock(m);
          mode = st.mode;
        }
        r = enet_host_service(host, &ev, 0);
      }

      // Refresh the player list (with round-trip times) for status().
      std::lock_guard lock(m);
      st.players.clear();
      for (const auto& e : roster) {
        std::uint32_t rtt = 0;
        if (st.mode == Mode::Hosting) {
          for (const auto& c : clients)
            if (c.id == e.id) rtt = c.peer->roundTripTime;
        } else if (server && e.id == kHostId) {
          rtt = server->roundTripTime;
        }
        st.players.push_back({e.id, to_string(e.name), rtt, e.role});
      }
    }
  }
};

Session::Session() : impl_(new Impl) {
  std::call_once(g_enet_once, [] { g_enet_ok = enet_initialize() == 0; });
}

Session::~Session() {
  leave();
  stop_recording();
  delete impl_;
}

bool Session::host(std::uint16_t port, const std::string& name, const std::string& bind_address) {
  impl_->stop();
  if (!g_enet_ok) {
    impl_->st.last_error = "ENet failed to initialise";
    return false;
  }
  ENetAddress addr{};
  addr.port = port;
  if (bind_address.empty() || bind_address == "0.0.0.0")
    addr.host = ENET_HOST_ANY;
  else if (enet_address_set_host(&addr, bind_address.c_str()) != 0) {
    impl_->st.last_error = "bad bind address " + bind_address;
    return false;
  }
  // One spare peer slot beyond the 7 joiners (3 co-op + 4 invaders): with none, ENet silently ignores an
  // extra connection attempt and that client just times out instead of hearing "session is full".
  impl_->host = enet_host_create(&addr, kMaxPlayers, kNumChannels, 0, 0);
  if (impl_->host) impl_->host->mtu = kMtu, impl_->host->maximumPacketSize = kMaxPacket;
  if (!impl_->host) {
    impl_->st.last_error = std::format("could not open UDP port {} (in use?)", port);
    return false;
  }
  {
    std::lock_guard lock(impl_->m);
    impl_->my_name = make_name(name);
    impl_->my_role = Role::Coop;
    impl_->roster = {{kHostId, impl_->my_name, Role::Coop}};
    impl_->st.my_role = Role::Coop;
    impl_->st.mode = Mode::Hosting;
    impl_->st.my_id = kHostId;
    impl_->st.port = port;
    impl_->st.last_error.clear();
  }
  impl_->invasion_open = false;
  impl_->start();
  return true;
}

bool Session::join(const std::string& address, std::uint16_t port, const std::string& name, Role role) {
  impl_->stop();
  if (!g_enet_ok) {
    impl_->st.last_error = "ENet failed to initialise";
    return false;
  }
  ENetAddress addr{};
  addr.port = port;
  if (enet_address_set_host(&addr, address.c_str()) != 0) {
    impl_->st.last_error = "could not resolve " + address;
    return false;
  }
  impl_->host = enet_host_create(nullptr, 1, kNumChannels, 0, 0);
  if (impl_->host) impl_->host->mtu = kMtu, impl_->host->maximumPacketSize = kMaxPacket;
  if (!impl_->host) {
    impl_->st.last_error = "could not create client socket";
    return false;
  }
  impl_->server = enet_host_connect(impl_->host, &addr, kNumChannels, kProtocolVersion);
  if (!impl_->server) {
    enet_host_destroy(impl_->host);
    impl_->host = nullptr;
    impl_->st.last_error = "connect failed";
    return false;
  }
  {
    std::lock_guard lock(impl_->m);
    impl_->my_name = make_name(name);
    impl_->my_role = role;
    impl_->st.my_role = role;
    impl_->st.mode = Mode::Connecting;
    impl_->st.port = port;
    impl_->st.last_error.clear();
  }
  impl_->start();
  return true;
}

void Session::set_invasion_open(bool open) { impl_->invasion_open = open; }

void Session::kick_invaders(Reject why) {
  std::lock_guard lock(impl_->m);
  impl_->kick_invaders_pending = true;
  impl_->kick_reason = why;
}

void Session::leave() {
  // Announce only a deliberate leave from a live session. After a remote disconnect the
  // mode is already Idle (that path raised its own event); host()/join() tear down via
  // stop() directly and must not raise one either, or a client that re-joins on
  // "disconnected" would loop forever on its own teardown.
  Mode mode;
  {
    std::lock_guard lock(impl_->m);
    mode = impl_->st.mode;
  }
  impl_->stop();
  if (mode != Mode::Idle) impl_->event(SessionEvent::Kind::Disconnected, 0, "left the session");
}

namespace {
// Recording record: u32 time, u8 reliable, u32 length, bytes. Game messages are recorded
// as sent; our own player/vehicle states as a marker byte + the raw struct, so a replay
// can move the host's stand-in along the recorded route.
void write_record(std::FILE* rec, std::uint32_t t, const std::uint8_t* data, std::uint32_t len, bool reliable) {
  const std::uint8_t rel = reliable ? 1 : 0;
  std::fwrite(&t, 4, 1, rec);
  std::fwrite(&rel, 1, 1, rec);
  std::fwrite(&len, 4, 1, rec);
  std::fwrite(data, 1, len, rec);
}

template <class T> void record_struct(std::FILE* rec, std::uint32_t t, std::uint8_t marker, const T& v) {
  std::uint8_t buf[1 + sizeof(T)];
  buf[0] = marker;
  std::memcpy(buf + 1, &v, sizeof(T));
  write_record(rec, t, buf, sizeof(buf), false);
}
} // namespace

void Session::send_state(const sync::PlayerState& s) {
  std::lock_guard lock(impl_->m);
  if (impl_->st.mode != Mode::Hosting && impl_->st.mode != Mode::Joined) return;
  if (impl_->rec) record_struct(impl_->rec, steady_ms() - impl_->rec_start_ms, kRecPlayerState, s);
  impl_->out_states.push_back(encode_state(impl_->st.my_id, s));
  while (impl_->out_states.size() > 8) impl_->out_states.pop_front();
}

void Session::send_vehicle(const sync::VehicleState& v) {
  std::lock_guard lock(impl_->m);
  if (impl_->st.mode != Mode::Hosting && impl_->st.mode != Mode::Joined) return;
  if (impl_->rec) record_struct(impl_->rec, steady_ms() - impl_->rec_start_ms, kRecVehicleState, v);
  impl_->out_states.push_back(encode_vehicle(impl_->st.my_id, v));
  while (impl_->out_states.size() > 8) impl_->out_states.pop_front();
}

void Session::send_game(std::vector<std::uint8_t> packet, bool reliable) {
  if (packet.size() < 2) return;
  std::lock_guard lock(impl_->m);
  if (impl_->st.mode != Mode::Hosting && impl_->st.mode != Mode::Joined) return;
  if (packet.size() > kMaxPacket) { // the receiver would refuse it; don't try
    ++impl_->dropped_out;
    if (impl_->logger && (impl_->dropped_out <= 5 || impl_->dropped_out % 100 == 0))
      impl_->logger(1, std::format("dropping an oversize message (type {}, {} bytes, limit {})", packet[0], packet.size(),
                                   kMaxPacket));
    return;
  }
  packet[1] = impl_->st.my_id;
  if (impl_->rec) {
    write_record(impl_->rec, steady_ms() - impl_->rec_start_ms, packet.data(),
                 static_cast<std::uint32_t>(packet.size()), reliable);
    ++impl_->rec_count;
  }
  impl_->out_game.push_back({std::move(packet), reliable});
  if (impl_->out_game.size() > 4096) impl_->out_game.pop_front(), ++impl_->dropped_out;
}

void Session::set_logger(LogFn fn) {
  std::lock_guard lock(impl_->m);
  impl_->logger = std::move(fn);
}

void Session::set_chaos(const Chaos& c) {
  std::lock_guard lock(impl_->m);
  impl_->chaos_cfg = c;
}

void Session::blackhole(std::uint32_t ms) {
  std::lock_guard lock(impl_->m);
  impl_->blackhole_until = steady_ms() + ms;
}

bool Session::start_recording(const std::string& path) {
  std::lock_guard lock(impl_->m);
  if (impl_->rec) std::fclose(impl_->rec);
  impl_->rec = std::fopen(path.c_str(), "wb");
  if (!impl_->rec) return false;
  const char magic[4] = {'I', 'V', 'C', 'R'};
  const std::uint32_t version = 1;
  std::fwrite(magic, 1, 4, impl_->rec);
  std::fwrite(&version, 4, 1, impl_->rec);
  impl_->rec_start_ms = steady_ms();
  impl_->rec_count = 0;
  return true;
}

void Session::stop_recording() {
  std::lock_guard lock(impl_->m);
  if (impl_->rec) std::fclose(impl_->rec);
  impl_->rec = nullptr;
}

bool Session::recording() const {
  std::lock_guard lock(impl_->m);
  return impl_->rec != nullptr;
}

std::vector<GameMessage> Session::drain_game() {
  std::lock_guard lock(impl_->m);
  std::vector<GameMessage> v(std::make_move_iterator(impl_->in_game.begin()),
                             std::make_move_iterator(impl_->in_game.end()));
  impl_->in_game.clear();
  return v;
}

std::vector<VehicleUpdate> Session::drain_vehicles() {
  std::lock_guard lock(impl_->m);
  std::vector<VehicleUpdate> v(impl_->in_vehicles.begin(), impl_->in_vehicles.end());
  impl_->in_vehicles.clear();
  return v;
}

std::vector<StateUpdate> Session::drain_states() {
  std::lock_guard lock(impl_->m);
  std::vector<StateUpdate> v(impl_->in_states.begin(), impl_->in_states.end());
  impl_->in_states.clear();
  return v;
}

std::vector<SessionEvent> Session::drain_events() {
  std::lock_guard lock(impl_->m);
  std::vector<SessionEvent> v(impl_->events.begin(), impl_->events.end());
  impl_->events.clear();
  return v;
}

SessionStatus Session::status() const {
  std::lock_guard lock(impl_->m);
  SessionStatus s = impl_->st;
  s.bytes_sent = impl_->total_out, s.bytes_received = impl_->total_in;
  s.dropped_in = impl_->dropped_in, s.dropped_out = impl_->dropped_out;
  return s;
}

} // namespace ivc::net
