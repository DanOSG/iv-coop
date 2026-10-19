#pragma once
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "net/bytes.h"

namespace ivc::sync {

enum class MsgType : std::uint8_t { PlayerState = 1 };

// On-foot player snapshot. Grows as sync covers more (pad state, aim, weapon, vehicle seat).
struct PlayerState {
  std::uint32_t time_ms = 0; // sender clock when captured
  std::uint16_t seq = 0;
  float x = 0, y = 0, z = 0;
  float vx = 0, vy = 0, vz = 0;
  float heading = 0; // degrees, 0..360
  std::uint32_t model = 0;
  std::uint8_t in_vehicle = 0;
};

inline std::vector<std::uint8_t> encode(const PlayerState& s) {
  net::Writer w;
  w.put(MsgType::PlayerState).put(s.seq).put(s.time_ms).put(s.x).put(s.y).put(s.z).put(s.vx).put(s.vy).put(s.vz)
      .put(s.heading).put(s.model).put(s.in_vehicle);
  return std::move(w.data());
}

inline std::optional<PlayerState> decode_player_state(std::span<const std::uint8_t> d) {
  net::Reader r(d);
  MsgType type{};
  PlayerState s;
  r.get(type);
  if (type != MsgType::PlayerState) return std::nullopt;
  r.get(s.seq), r.get(s.time_ms), r.get(s.x), r.get(s.y), r.get(s.z), r.get(s.vx), r.get(s.vy), r.get(s.vz);
  r.get(s.heading), r.get(s.model), r.get(s.in_vehicle);
  if (!r.ok()) return std::nullopt;
  return s;
}

// Snapshot interpolation: render remote state `delay_ms` in the past, lerping between the
// two snapshots that bracket that time (or extrapolating briefly with velocity).
class Interpolator {
 public:
  std::uint32_t delay_ms = 100;
  std::uint32_t max_extrapolate_ms = 250;

  void push(const PlayerState& s) {
    if (!buf_.empty() && static_cast<std::int16_t>(s.seq - buf_.back().seq) <= 0) {
      ++dropped_out_of_order_;
      return; // stale or duplicate
    }
    buf_.push_back(s);
    while (buf_.size() > 64) buf_.pop_front();
  }

  // `remote_now_ms` is the receiver's estimate of the sender's clock.
  std::optional<PlayerState> sample(std::uint32_t remote_now_ms) const {
    if (buf_.empty()) return std::nullopt;
    const std::uint32_t t = remote_now_ms - delay_ms;
    if (static_cast<std::int32_t>(t - buf_.front().time_ms) <= 0) return buf_.front();
    for (std::size_t i = 1; i < buf_.size(); ++i) {
      const PlayerState& a = buf_[i - 1];
      const PlayerState& b = buf_[i];
      if (static_cast<std::int32_t>(t - b.time_ms) <= 0) {
        const float span = static_cast<float>(b.time_ms - a.time_ms);
        const float k = span > 0 ? static_cast<float>(t - a.time_ms) / span : 1.0f;
        PlayerState o = b;
        o.x = a.x + (b.x - a.x) * k;
        o.y = a.y + (b.y - a.y) * k;
        o.z = a.z + (b.z - a.z) * k;
        o.heading = lerp_angle(a.heading, b.heading, k);
        return o;
      }
    }
    PlayerState o = buf_.back(); // newer than anything we have: extrapolate a little
    const float dt = static_cast<float>(std::min(t - o.time_ms, max_extrapolate_ms)) / 1000.0f;
    o.x += o.vx * dt, o.y += o.vy * dt, o.z += o.vz * dt;
    return o;
  }

  void clear() { buf_.clear(); }
  std::size_t buffered() const { return buf_.size(); }
  std::uint32_t dropped() const { return dropped_out_of_order_; }

 private:
  static float lerp_angle(float a, float b, float k) {
    float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
    return std::fmod(a + d * k + 360.0f, 360.0f);
  }
  std::deque<PlayerState> buf_;
  std::uint32_t dropped_out_of_order_ = 0;
};

} // namespace ivc::sync
