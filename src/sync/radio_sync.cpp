#include "sync/radio_sync.h"

#include <windows.h>

#include <array>
#include <cstdlib>
#include <map>

#include "core/log.h"
#include "game/radio.h"
#include "net/bytes.h"

namespace ivc::radio_sync {
namespace {

constexpr int kMaxStations = 32;
constexpr DWORD kKeyframeMs = 3000, kStationSendMs = 1000;
constexpr std::int64_t kAudibleTolerance = 1000, kSilentTolerance = 500;
constexpr DWORD kStaleMs = 10000;

struct Remote {
  radio::Item item;
  std::uint32_t position = 0;
  DWORD received = 0;
};

// Host.
std::array<radio::Item, kMaxStations> g_sent{};
DWORD g_last_keyframe = 0;
// Everyone: our tuned station as last sent.
int g_sent_station = -2;
DWORD g_last_station_send = 0;
// Joiner.
std::array<Remote, kMaxStations> g_host{};
int g_host_count = 0;
bool g_pending = false;
std::map<std::uint8_t, int> g_player_station; // player id -> their tuned station (player numbering)
std::map<std::uint8_t, DWORD> g_changed_at;    // player id -> when we saw their station change
int g_my_station = -2;
DWORD g_my_changed = 0;

void send_timeline(net::Session& net, int count, bool reliable) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(std::uint8_t{0}).put(static_cast<std::uint8_t>(count));
  for (int i = 0; i < count; ++i) {
    const radio::StationState s = radio::read(i);
    w.put(s.item.category).put(s.item.track).put(s.position);
    g_sent[i] = s.item;
  }
  net.send_game(std::move(w.data()), reliable);
}

void send_station(net::Session& net, int station) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(std::uint8_t{1}).put(static_cast<std::uint8_t>(station & 0xFF));
  net.send_game(std::move(w.data()), true);
}

void host_tick(net::Session& net, DWORD now) {
  const int count = std::min(radio::station_count(), kMaxStations);
  if (count <= 0) return;
  bool changed = false;
  for (int i = 0; i < count && !changed; ++i) changed = radio::read(i).item != g_sent[i];
  if (changed || now - g_last_keyframe >= kKeyframeMs) {
    g_last_keyframe = now;
    send_timeline(net, count, changed);
  }
}

void client_apply(DWORD now) {
  if (!g_pending) return;
  g_pending = false;
  const int count = std::min({radio::station_count(), g_host_count, kMaxStations});
  for (int i = 0; i < count; ++i) {
    const Remote& h = g_host[i];
    if (h.item.category == 0xFF || now - h.received > kStaleMs) continue;
    const std::uint32_t expected = h.position + (now - h.received);
    const radio::StationState local = radio::read(i);
    if (!local.valid) continue;
    const std::int64_t drift = static_cast<std::int64_t>(local.position) - expected;
    if (local.item != h.item) {
      if (radio::play(i, h.item, expected, local.audible))
        log::debug("Radio: station {} -> cat {} #{} at {} ms{}", i, h.item.category, h.item.track, expected,
                   local.audible ? " (audible)" : "");
    } else if (std::llabs(drift) > (local.audible ? kAudibleTolerance : kSilentTolerance)) {
      if (local.audible)
        radio::play(i, h.item, expected, true);
      else
        radio::set_idle_position(i, expected);
    }
  }
}

// Front seats choose (the one who changed it last; the driver by default), the back follows.
void follow_front(const Frame& f, DWORD now) {
  if (!f.in_car) return;
  const int mine = radio::player_station();
  if (mine != g_my_station) g_my_station = mine, g_my_changed = now;
  std::uint8_t chooser = 0xFF;
  DWORD latest = 0;
  for (const std::uint8_t id : {f.driver_id, f.front_id}) {
    if (id == 0xFF) continue;
    const auto it = g_changed_at.find(id);
    const DWORD at = it != g_changed_at.end() ? it->second : 0;
    if (chooser == 0xFF || at > latest) chooser = id, latest = at;
  }
  if (chooser == 0xFF) return;
  if (f.front_seat && g_my_changed >= latest) return; // our own change is the latest: the others follow us
  const auto it = g_player_station.find(chooser);
  if (it == g_player_station.end() || it->second < 0 || it->second >= 255) return;
  if (mine != it->second) radio::retune(it->second), g_my_station = it->second;
}

} // namespace

void tick(const Frame& f, net::Session& net) {
  if (!f.in_session) {
    clear();
    return;
  }
  const DWORD now = GetTickCount();
  if (f.hosting) host_tick(net, now);
  else client_apply(now);

  const int station = radio::player_station();
  if (station != g_sent_station || now - g_last_station_send >= kStationSendMs * 5) {
    g_sent_station = station, g_last_station_send = now;
    send_station(net, station);
  }
  follow_front(f, now);
}

void on_message(const net::GameMessage& m) {
  net::Reader r(m.data);
  std::uint8_t type, sender, kind, value;
  if (!r.get(type) || !r.get(sender) || !r.get(kind) || !r.get(value)) return;
  if (kind == 1) {
    const auto old = g_player_station.find(m.from);
    if (old == g_player_station.end() || old->second != value) g_changed_at[m.from] = GetTickCount();
    g_player_station[m.from] = value;
    return;
  }
  if (kind != 0 || m.from != net::kHostId) return;
  const int count = std::min<int>(value, kMaxStations);
  std::array<Remote, kMaxStations> in{};
  const DWORD now = GetTickCount();
  for (int i = 0; i < count; ++i) {
    if (!r.get(in[i].item.category) || !r.get(in[i].item.track) || !r.get(in[i].position)) return;
    in[i].received = now;
  }
  g_host = in, g_host_count = count, g_pending = true;
}

void clear() {
  g_sent.fill({});
  g_host.fill({});
  g_host_count = 0;
  g_pending = false;
  g_sent_station = -2;
  g_player_station.clear();
  g_changed_at.clear();
  g_my_station = -2, g_my_changed = 0;
}

} // namespace ivc::radio_sync
