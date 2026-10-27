#include "sync/regroup.h"

#include <windows.h>

#include <cmath>
#include <optional>
#include <string>

#include "core/log.h"
#include "game/phone.h"
#include "net/bytes.h"

namespace ivc::regroup {
namespace {
namespace n = natives;
using n::invoke;

constexpr std::uint32_t kGetMissionFlag = 0x2BC64736, kIsScreenFadedOut = 0x59EE3A11, kLoadScene = 0x39F62BFB,
                        kRequestCollisionAt = 0x12ED0BC9, kGetSafePosition = 0x5D877285, kGetGroundZ = 0x6D902EE3,
                        kNthClosestCarNode = 0x1F6B3FF0, kSetCarHeading = 0x75E40528,
                        kSetCarOnGroundProperly = 0x0E717E98;
constexpr float kJumpM = 40.0f;      // host moved this far in one frame: a script put it there
constexpr float kNearM = 25.0f;      // partners this close are left alone
constexpr DWORD kSettleMs = 1500;    // after the trigger, before the host sends
constexpr DWORD kCooldownMs = 4000;  // between host sends
constexpr DWORD kWaitPuppetMs = 4000; // joiner: for the host's copy to reach the new spot

enum class Reason : std::uint8_t { Teleport = 0, FadeIn = 1, MissionStart = 2 };
const char* reason_name(Reason r) {
  return r == Reason::Teleport ? "host was moved" : r == Reason::FadeIn ? "scene ended" : "mission started";
}

struct Where {
  n::Vector3 at{};
  float heading = 0;
  bool in_car = false;
  std::uint32_t car_model = 0;
  Reason reason = Reason::Teleport;
};

// Host.
std::optional<n::Vector3> g_last_pos;
bool g_was_faded = false, g_was_mission = false;
std::optional<Reason> g_trigger;
DWORD g_trigger_ms = 0, g_last_send_ms = 0;
// Joiner.
std::optional<Where> g_pending;
DWORD g_pending_ms = 0;
// A car we're waiting on the model for (joiner, host driving, no free seat, we're on foot).
std::uint32_t g_wanted_model = 0;

float dist(const n::Vector3& a, const n::Vector3& b) {
  const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void host_tick(net::Session& net, DWORD now) {
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me) return;
  const n::Vector3 pos = n::GetCharCoordinates(me);
  const bool faded = invoke<bool>(kIsScreenFadedOut), mission = invoke<bool>(kGetMissionFlag);
  std::optional<Reason> r;
  if (g_last_pos && dist(pos, *g_last_pos) > kJumpM) r = Reason::Teleport;
  else if (g_was_faded && !faded) r = Reason::FadeIn;
  else if (!g_was_mission && mission) r = Reason::MissionStart;
  g_last_pos = pos, g_was_faded = faded, g_was_mission = mission;
  if (r && !g_trigger) g_trigger = r, g_trigger_ms = now;

  if (!g_trigger || now - g_trigger_ms < kSettleMs || now - g_last_send_ms < kCooldownMs) return;
  Where w;
  w.at = pos, w.heading = n::GetCharHeading(me), w.reason = *g_trigger;
  if (n::IsCharInAnyCar(me))
    if (const n::Vehicle car = n::StoreCarCharIsInNoSave(me)) w.in_car = true, w.car_model = n::GetCarModel(car);
  g_trigger.reset(), g_last_send_ms = now;
  net::Writer wr;
  wr.put(kMsg).put(std::uint8_t{0}).put(w.at.x).put(w.at.y).put(w.at.z).put(w.heading)
      .put(static_cast<std::uint8_t>(w.in_car)).put(w.car_model).put(static_cast<std::uint8_t>(w.reason));
  net.send_game(std::move(wr.data()), true);
  log::info("Regroup: {} - partners come to {:.0f} {:.0f} {:.0f}{}", reason_name(w.reason), w.at.x, w.at.y, w.at.z,
            w.in_car ? " (driving)" : "");
}

// Somewhere safe to stand near `around`: collision loaded first, then the game's own safe-spot search.
n::Vector3 safe_spot(n::Vector3 around) {
  invoke(kRequestCollisionAt, around.x, around.y, around.z);
  invoke(kLoadScene, around.x, around.y, around.z);
  n::Vector3 out{};
  if (invoke<bool>(kGetSafePosition, around.x, around.y, around.z, true, &out.x, &out.y, &out.z) &&
      dist(out, around) < 30.0f)
    return out;
  float ground = 0;
  if (invoke<bool>(kGetGroundZ, around.x, around.y, around.z + 2.0f, &ground) && ground != 0)
    return {around.x, around.y, ground + 0.5f};
  return {around.x, around.y, around.z + 0.5f};
}

// The road node behind `w` (a few nodes away, so we don't land on the host's car).
bool road_spot(const Where& w, n::Vector3& at, float& heading) {
  invoke(kRequestCollisionAt, w.at.x, w.at.y, w.at.z);
  invoke(kLoadScene, w.at.x, w.at.y, w.at.z);
  for (std::uint32_t nth = 3; nth <= 8; ++nth) {
    if (!invoke<bool>(kNthClosestCarNode, w.at.x, w.at.y, w.at.z, nth, &at.x, &at.y, &at.z, &heading)) continue;
    if (dist(at, w.at) > 6.0f && dist(at, w.at) < 60.0f) return true;
  }
  return false;
}

// Puts us where `w` says. False: not done yet (a model loading); try again next frame.
bool apply(const Where& w, std::uint8_t my_id, n::Ped host) {
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me || n::IsCharDead(me)) return true;
  const n::Vector3 my_pos = n::GetCharCoordinates(me);
  const bool i_drive = n::IsCharInAnyCar(me);
  const n::Vehicle host_car = host && n::IsCharInAnyCar(host) ? n::StoreCarCharIsInNoSave(host) : 0;
  if (w.in_car) {
    if (host_car && n::IsCharInCar(me, host_car)) return true;
    if (dist(my_pos, w.at) < kNearM && i_drive) return true; // close and driving: they can follow themselves
    if (host_car) {
      const std::uint32_t seats = n::GetMaximumNumberOfPassengers(host_car);
      for (std::uint32_t seat = 0; seat < seats && seat < 8; ++seat)
        if (n::IsCarPassengerSeatFree(host_car, seat)) {
          if (i_drive) n::WarpCharFromCarToCoord(me, my_pos);
          n::WarpCharIntoCarAsPassenger(me, host_car, seat);
          log::info("Regroup: into the host's car, seat {}", seat);
          return true;
        }
    }
    n::Vector3 at{};
    float heading = w.heading;
    if (!road_spot(w, at, heading)) at = safe_spot({w.at.x - 6.0f, w.at.y, w.at.z});
    if (i_drive) {
      const n::Vehicle mine = n::StoreCarCharIsInNoSave(me);
      n::SetCarCoordinatesNoOffset(mine, at);
      invoke(kSetCarHeading, mine, heading);
      invoke(kSetCarOnGroundProperly, mine);
      log::info("Regroup: brought our car to the road behind the host");
      return true;
    }
    const std::uint32_t model = w.car_model;
    if (!model) return true;
    n::RequestModel(model);
    if (!n::HasModelLoaded(model)) {
      g_wanted_model = model;
      return false;
    }
    const n::Vehicle car = n::CreateCar(model, at);
    n::MarkModelAsNoLongerNeeded(model);
    g_wanted_model = 0;
    if (!car) return true;
    invoke(kSetCarHeading, car, heading);
    invoke(kSetCarOnGroundProperly, car);
    n::WarpCharIntoCar(me, car);
    log::info("Regroup: in a car of our own behind the host");
    return true;
  }
  if (dist(my_pos, w.at) < kNearM && !i_drive) return true;
  // Each partner at their own spot around the host: behind-left, behind-right, behind.
  const float side = (my_id % 3 == 1) ? -2.0f : (my_id % 3 == 2) ? 2.0f : 0.0f;
  const float h = w.heading * 3.14159265f / 180.0f;
  // IV heading: forward is (-sin h, cos h), right is (cos h, sin h); 2.5 m behind, `side` to the right.
  const n::Vector3 around{w.at.x + std::sin(h) * 2.5f + std::cos(h) * side, w.at.y - std::cos(h) * 2.5f + std::sin(h) * side,
                          w.at.z};
  const n::Vector3 at = safe_spot(around);
  if (i_drive) n::WarpCharFromCarToCoord(me, at);
  n::SetCharCoordinatesNoOffset(me, at);
  n::SetCharHeading(me, w.heading);
  log::info("Regroup: next to the host at {:.0f} {:.0f} {:.0f}", at.x, at.y, at.z);
  return true;
}

void client_tick(const Frame& f, DWORD now) {
  if (!g_pending) return;
  // Wait for our copy of the host to arrive (the seat is in its car), up to kWaitPuppetMs.
  const bool host_there = f.host_puppet && n::DoesCharExist(f.host_puppet) &&
                          dist(n::GetCharCoordinates(f.host_puppet), g_pending->at) < 15.0f;
  if (!host_there && now - g_pending_ms < kWaitPuppetMs) return;
  if (apply(*g_pending, f.my_id, host_there ? f.host_puppet : 0)) {
    phone::notify(std::string("Regrouped with the host: ") + reason_name(g_pending->reason) + ".");
    g_pending.reset();
  } else if (now - g_pending_ms > 15000) {
    g_pending.reset(); // the model never loaded
  }
}

} // namespace

void tick(const Frame& f, net::Session& net) {
  if (!f.in_session) {
    clear();
    return;
  }
  const DWORD now = GetTickCount();
  if (f.hosting) host_tick(net, now);
  else client_tick(f, now);
}

void on_message(const net::GameMessage& m) {
  if (m.from != net::kHostId) return;
  net::Reader r(m.data);
  std::uint8_t type = 0, sender = 0, in_car = 0, reason = 0;
  Where w;
  if (!r.get(type) || !r.get(sender) || !r.get(w.at.x) || !r.get(w.at.y) || !r.get(w.at.z) || !r.get(w.heading) ||
      !r.get(in_car) || !r.get(w.car_model) || !r.get(reason))
    return;
  w.in_car = in_car != 0, w.reason = static_cast<Reason>(reason);
  g_pending = w, g_pending_ms = GetTickCount();
}

void clear() {
  g_last_pos.reset();
  g_was_faded = g_was_mission = false;
  g_trigger.reset();
  g_pending.reset();
  if (g_wanted_model) n::MarkModelAsNoLongerNeeded(g_wanted_model), g_wanted_model = 0;
}

} // namespace ivc::regroup
