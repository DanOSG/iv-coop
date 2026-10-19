#include "net/session.h"

#include <enet/enet.h>

#include <chrono>
#include <format>

namespace ivc::net {
namespace {

std::once_flag g_enet_once;
bool g_enet_ok = false;

const char* reason_text(std::uint32_t r) {
  switch (static_cast<Reject>(r)) {
    case Reject::Full: return "session is full";
    case Reject::VersionMismatch: return "protocol version mismatch";
    case Reject::HostLeft: return "host closed the session";
    case Reject::Kicked: return "kicked";
    default: return "connection lost";
  }
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
    bool greeted = false;
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
  std::deque<std::vector<std::uint8_t>> out_states;
  std::deque<StateUpdate> in_states;
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
    st = {};
  }

  void event(SessionEvent::Kind k, std::uint8_t id, std::string text) {
    std::lock_guard lock(m);
    events.push_back({k, id, std::move(text)});
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

  // --- host side ---
  void host_on_connect(ENetEvent& ev) {
    if (ev.data != kProtocolVersion) {
      enet_peer_disconnect(ev.peer, static_cast<std::uint32_t>(Reject::VersionMismatch));
      return;
    }
    for (std::uint8_t id = 1; id < kMaxPlayers; ++id) {
      bool used = false;
      for (const auto& c : clients) used |= c.id == id;
      if (!used) {
        clients.push_back({ev.peer, id});
        return; // announced once their Hello arrives
      }
    }
    enet_peer_disconnect(ev.peer, static_cast<std::uint32_t>(Reject::Full));
  }

  void host_on_receive(ENetEvent& ev) {
    Client* c = client_by_peer(ev.peer);
    if (!c) return;
    std::span<std::uint8_t> data(ev.packet->data, ev.packet->dataLength);
    Reader r(data);
    MsgType type{};
    r.get(type);

    if (type == MsgType::Hello && !c->greeted) {
      std::uint16_t version = 0;
      r.get(version), r.get(c->name);
      if (!r.ok() || version != kProtocolVersion) {
        enet_peer_disconnect(ev.peer, static_cast<std::uint32_t>(Reject::VersionMismatch));
        return;
      }
      c->greeted = true;
      std::vector<RosterEntry> snapshot;
      {
        std::lock_guard lock(m);
        roster.push_back({c->id, c->name});
        snapshot = roster;
      }
      send_packet(ev.peer, kChanControl, encode_welcome(c->id, snapshot), true);
      const auto joined = encode_joined(c->id, c->name);
      for (auto& other : clients)
        if (other.greeted && other.peer != ev.peer) send_packet(other.peer, kChanControl, joined, true);
      event(SessionEvent::Kind::Joined, c->id, to_string(c->name));
    } else if (type == MsgType::PlayerState && c->greeted) {
      std::uint8_t id;
      sync::PlayerState s;
      if (!decode_state(r, id, s)) return;
      push_state(c->id, s);
      restamp_state(data, c->id); // never trust the sender's id
      for (auto& other : clients)
        if (other.greeted && other.peer != ev.peer) send_packet(other.peer, kChanState, data, false);
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
          if (other.greeted && other.peer != ev.peer) send_packet(other.peer, kChanControl, left, true);
        event(SessionEvent::Kind::Left, id, to_string(it->name));
      }
      clients.erase(it);
      return;
    }
  }

  // --- client side ---
  void client_on_receive(ENetEvent& ev) {
    Reader r(std::span<const std::uint8_t>(ev.packet->data, ev.packet->dataLength));
    MsgType type{};
    r.get(type);
    if (type == MsgType::Welcome) {
      std::uint8_t my_id = 0, count = 0;
      r.get(my_id), r.get(count);
      std::vector<RosterEntry> list(count);
      for (auto& e : list) r.get(e.id), r.get(e.name);
      if (!r.ok()) return;
      {
        std::lock_guard lock(m);
        roster = list;
        st.my_id = my_id;
        st.mode = Mode::Joined;
      }
      event(SessionEvent::Kind::Connected, my_id, {});
      for (const auto& e : list)
        if (e.id != my_id) event(SessionEvent::Kind::Joined, e.id, to_string(e.name));
    } else if (type == MsgType::PlayerJoined) {
      RosterEntry e{};
      r.get(e.id), r.get(e.name);
      if (!r.ok()) return;
      {
        std::lock_guard lock(m);
        roster.push_back(e);
      }
      event(SessionEvent::Kind::Joined, e.id, to_string(e.name));
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
    } else if (type == MsgType::PlayerState) {
      std::uint8_t id;
      sync::PlayerState s;
      if (decode_state(r, id, s) && id != st.my_id) push_state(id, s);
    }
  }

  void loop() {
    while (running) {
      std::deque<std::vector<std::uint8_t>> out;
      Mode mode;
      {
        std::lock_guard lock(m);
        out.swap(out_states);
        mode = st.mode;
      }
      for (const auto& pkt : out) {
        if (mode == Mode::Hosting) {
          for (auto& c : clients)
            if (c.greeted) send_packet(c.peer, kChanState, pkt, false);
        } else if (mode == Mode::Joined && server) {
          send_packet(server, kChanState, pkt, false);
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
              send_packet(ev.peer, kChanControl, encode_hello(to_string(my_name)), true);
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
              const char* why = reason_text(ev.data);
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
        st.players.push_back({e.id, to_string(e.name), rtt});
      }
    }
  }
};

Session::Session() : impl_(new Impl) {
  std::call_once(g_enet_once, [] { g_enet_ok = enet_initialize() == 0; });
}

Session::~Session() {
  leave();
  delete impl_;
}

bool Session::host(std::uint16_t port, const std::string& name, const std::string& bind_address) {
  leave();
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
  // One spare peer slot beyond the 3 joiners: with none, ENet silently ignores a 4th
  // connection attempt and that client just times out instead of hearing "session is full".
  impl_->host = enet_host_create(&addr, kMaxPlayers, kNumChannels, 0, 0);
  if (!impl_->host) {
    impl_->st.last_error = std::format("could not open UDP port {} (in use?)", port);
    return false;
  }
  {
    std::lock_guard lock(impl_->m);
    impl_->my_name = make_name(name);
    impl_->roster = {{kHostId, impl_->my_name}};
    impl_->st.mode = Mode::Hosting;
    impl_->st.my_id = kHostId;
    impl_->st.port = port;
    impl_->st.last_error.clear();
  }
  impl_->start();
  return true;
}

bool Session::join(const std::string& address, std::uint16_t port, const std::string& name) {
  leave();
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
    impl_->st.mode = Mode::Connecting;
    impl_->st.port = port;
    impl_->st.last_error.clear();
  }
  impl_->start();
  return true;
}

void Session::leave() {
  const bool was_active = impl_->host != nullptr;
  impl_->stop();
  if (was_active) impl_->event(SessionEvent::Kind::Disconnected, 0, "left the session");
}

void Session::send_state(const sync::PlayerState& s) {
  std::lock_guard lock(impl_->m);
  if (impl_->st.mode != Mode::Hosting && impl_->st.mode != Mode::Joined) return;
  impl_->out_states.push_back(encode_state(impl_->st.my_id, s));
  while (impl_->out_states.size() > 4) impl_->out_states.pop_front();
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
  return impl_->st;
}

} // namespace ivc::net
