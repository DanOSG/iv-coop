#pragma once
// Snapshot interpolation for any replicated state type S.
//
// The sender stamps each snapshot with its own clock; the receiver learns `offset` =
// (local arrival - sender stamp), i.e. clock difference + one-way latency, from the fastest
// packets in a recent window, and renders the sender's timeline at
// (local now - offset - delay_ms). `delay_ms` is the jitter buffer: it must exceed jitter +
// one send interval so there is almost always a snapshot on each side of the render time.
//
// S must have `std::uint16_t seq`, `std::uint32_t time_ms`, and provide
//   static S lerp(const S& a, const S& b, float k);        // k in [0,1]
//   static S extrapolate(const S& s, float seconds);

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace ivc::sync {

template <class S>
class SnapshotBuffer {
 public:
  std::uint32_t delay_ms = 100;
  std::uint32_t max_extrapolate_ms = 250;
  std::uint32_t offset_window_ms = 2000;

  struct Stats {
    std::uint32_t interpolated = 0, extrapolated = 0, stale = 0;
    std::int32_t offset_ms = 0; // smoothed (arrival - sender time)
  };

  void push(const S& s, std::uint32_t local_arrival_ms) {
    if (!buf_.empty() && static_cast<std::int16_t>(s.seq - buf_.back().seq) <= 0) {
      ++stats_.stale;
      return; // reordered or duplicate: older than what we already have
    }
    buf_.push_back(s);
    while (buf_.size() > 64) buf_.pop_front();

    // Windowed minimum of the offset: the fastest recent packet best reflects the clock
    // difference + base latency; slower ones are jitter, which delay_ms absorbs.
    offsets_.push_back({local_arrival_ms, static_cast<std::int32_t>(local_arrival_ms - s.time_ms)});
    while (!offsets_.empty() && local_arrival_ms - offsets_.front().first > offset_window_ms) offsets_.pop_front();
    std::int32_t target = offsets_.front().second;
    for (const auto& o : offsets_) target = std::min(target, o.second);
    if (!have_offset_) smoothed_offset_ = static_cast<float>(target), have_offset_ = true;
    target_offset_ = target;
    last_arrival_ms_ = local_arrival_ms;
  }

  // Call once per frame: sampling advances the timeline and the stats.
  std::optional<S> sample(std::uint32_t local_now_ms) {
    if (buf_.empty() || !have_offset_) return std::nullopt;
    // Glide toward the target offset so window changes never make the timeline jump.
    smoothed_offset_ += (static_cast<float>(target_offset_) - smoothed_offset_) * 0.05f;
    stats_.offset_ms = static_cast<std::int32_t>(smoothed_offset_);

    const std::uint32_t t = local_now_ms - static_cast<std::uint32_t>(stats_.offset_ms) - delay_ms;
    while (buf_.size() > 2 && static_cast<std::int32_t>(t - buf_[1].time_ms) > 0) buf_.pop_front();

    if (static_cast<std::int32_t>(t - buf_.front().time_ms) <= 0) return buf_.front();
    for (std::size_t i = 1; i < buf_.size(); ++i) {
      const S& a = buf_[i - 1];
      const S& b = buf_[i];
      if (static_cast<std::int32_t>(t - b.time_ms) <= 0) {
        const float span = static_cast<float>(b.time_ms - a.time_ms);
        const float k = span > 0 ? static_cast<float>(t - a.time_ms) / span : 1.0f;
        ++stats_.interpolated;
        return S::lerp(a, b, k);
      }
    }
    // Render time is past our newest snapshot: extrapolate briefly.
    const S& last = buf_.back();
    const float dt = static_cast<float>(std::min(t - last.time_ms, max_extrapolate_ms)) / 1000.0f;
    ++stats_.extrapolated;
    return S::extrapolate(last, dt);
  }

  void clear() {
    buf_.clear();
    offsets_.clear();
    have_offset_ = false;
    stats_ = {};
  }
  std::size_t buffered() const { return buf_.size(); }
  const Stats& stats() const { return stats_; }
  // Local arrival time of the newest snapshot (0 if none yet).
  std::uint32_t last_arrival_ms() const { return last_arrival_ms_; }

 private:
  std::deque<S> buf_;
  std::deque<std::pair<std::uint32_t, std::int32_t>> offsets_;
  bool have_offset_ = false;
  std::int32_t target_offset_ = 0;
  float smoothed_offset_ = 0;
  std::uint32_t last_arrival_ms_ = 0;
  Stats stats_;
};

} // namespace ivc::sync
