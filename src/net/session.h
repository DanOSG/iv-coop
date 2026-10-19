#pragma once
// Host/join session over ENet. All ENet calls happen on a private network thread, so the
// connection survives the game pausing its script update (e.g. when the window loses focus).
// The game thread (or the bot's main loop) talks to it only through the queues below.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net/protocol.h"
#include "sync/player_state.h"

namespace ivc::net {

enum class Mode { Idle, Hosting, Connecting, Joined };

struct PeerStatus {
  std::uint8_t id;
  std::string name;
  std::uint32_t rtt_ms; // 0 for ourselves
};

struct SessionStatus {
  Mode mode = Mode::Idle;
  std::uint8_t my_id = 0;
  std::uint16_t port = 0;
  std::vector<PeerStatus> players; // everyone in the session, including us
  std::string last_error;
  std::uint32_t states_sent = 0, states_received = 0;
};

struct StateUpdate {
  std::uint8_t id;
  sync::PlayerState state;
  std::uint32_t arrival_ms; // receiver's clock (steady ms)
};

struct SessionEvent {
  enum class Kind { Joined, Left, Connected, Disconnected } kind;
  std::uint8_t id = 0; // Joined/Left
  std::string text;    // name, or disconnect reason
};

// Milliseconds on the steady clock; the timebase for all StateUpdate::arrival_ms.
std::uint32_t steady_ms();

class Session {
 public:
  Session();
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  bool host(std::uint16_t port, const std::string& name, const std::string& bind_address = "0.0.0.0");
  bool join(const std::string& address, std::uint16_t port, const std::string& name);
  void leave();

  // Queues our latest state; the network thread sends it at once (unreliable).
  void send_state(const sync::PlayerState& s);
  std::vector<StateUpdate> drain_states();
  std::vector<SessionEvent> drain_events();
  SessionStatus status() const;

 private:
  struct Impl;
  Impl* impl_;
};

const char* to_string(Mode m);

} // namespace ivc::net
