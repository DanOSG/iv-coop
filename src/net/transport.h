#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <random>
#include <span>
#include <vector>

namespace ivc::net {

enum class Channel : std::uint8_t { Unreliable = 0, Reliable = 1 };

// Peer 0 is always the host. The sync code only ever talks to this interface, so the
// loopback (M1), ENet (M2) and any later relay transport are interchangeable.
class Transport {
 public:
  using Handler = std::function<void(int peer, std::span<const std::uint8_t> payload)>;
  virtual ~Transport() = default;
  virtual void send(int peer, Channel ch, std::span<const std::uint8_t> payload) = 0;
  // Delivers everything due by `now_ms` to `on_receive`.
  virtual void poll(std::uint32_t now_ms, const Handler& on_receive) = 0;
};

// In-process transport for testing sync without a second machine: everything sent comes
// back to the sender after `latency_ms` (+ jitter), and unreliable packets may be dropped.
class LoopbackTransport final : public Transport {
 public:
  std::uint32_t latency_ms = 200;
  std::uint32_t jitter_ms = 0;
  float loss = 0.0f; // unreliable channel only

  void set_clock(std::uint32_t now_ms) { now_ = now_ms; }

  void send(int peer, Channel ch, std::span<const std::uint8_t> payload) override {
    if (ch == Channel::Unreliable && loss > 0.0f && dist_(rng_) < loss) return;
    const std::uint32_t jitter = jitter_ms ? static_cast<std::uint32_t>(dist_(rng_) * jitter_ms) : 0;
    queue_.push_back({now_ + latency_ms + jitter, peer, {payload.begin(), payload.end()}});
  }

  void poll(std::uint32_t now_ms, const Handler& on_receive) override {
    now_ = now_ms;
    // Jitter can reorder packets, like real UDP; scan the whole queue.
    for (auto it = queue_.begin(); it != queue_.end();) {
      if (it->due <= now_ms) {
        on_receive(it->peer, it->data);
        it = queue_.erase(it);
      } else {
        ++it;
      }
    }
  }

 private:
  struct Packet {
    std::uint32_t due;
    int peer;
    std::vector<std::uint8_t> data;
  };
  std::deque<Packet> queue_;
  std::uint32_t now_ = 0;
  std::mt19937 rng_{1234};
  std::uniform_real_distribution<float> dist_{0.0f, 1.0f};
};

} // namespace ivc::net
