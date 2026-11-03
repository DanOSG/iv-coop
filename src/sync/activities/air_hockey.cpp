#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include "core/log.h"
#include "game/episode.h"
#include "game/native_hooks.h"
#include "game/phone.h"
#include "sync/activities/activities.h"
#include "sync/activity_seats.h"

// TLAD + TBoGT air hockey as a 1v1 between two humans, from the host's own script (docs/activities/tlad_minigames.md,
// research\GTAIVMissionScripts\TLAD\air_hockey.txt and TBOGT\air_hockey.txt; line numbers below are TLAD's).
//
// The script has exactly two seats: seat 0 is the host's player (paddle l_U254[0], input read from pad 0 by sub_14382
// :2066) and seat 1 is the ped l_U524 with a paddle driven by the AI (sub_15445 :2164), which runs unless static 486 is
// above zero (:2027-2036: it is only ever decremented). When a game is under way (l_U245 == 4) and a partner is near:
//   - their puppet takes the opponent seat (l_U524) exactly as the script seats the guard / a friend (hidden original
//     kept as the stand-in, attached to the same anchor object l_U538[1]);
//   - the AI is held off (l_U486 := 1e9 each frame);
//   - every frame, while the sim runs (states 4 and 8), paddle 1's velocity statics get what the seat-0 path would
//     write for the partner's input, mirrored for the far seat: the same helper sub_14423 (:2115) with its table-bound
//     gates, the same right-stick slam (:2072-2103) and mouse fallback (:2108-2114), and the paddle/puck wall pinch
//     of sub_14980 (:2126) mirrored. The partner's sticks come from activity_seats::sample (seat on a pad nobody reads).
// The puck, paddles and score are the host's script; the objects stream to partners as precise entities.
// TBoGT is the same script with every static shifted by -113.
namespace ivc::activities::air_hockey {
namespace {
namespace n = natives;

constexpr std::uint32_t kAttachPedToObject = 0x376917AB, kDetachPed = 0x2CD52C5C;
constexpr std::uint8_t kNobody = 0xFF;
constexpr int kPad = 97; // no script reads it: the seat just streams input all the time

// TLAD statics (l_U<n> = statics[n]); TBoGT = these - 113 (checked for every slot used, see the layout test).
constexpr int kWords = 600;            // highest named: l_U594
constexpr int kState = 245;            // 3 setup, 4 play, 5/6 goal, 7 rematch, 8 pause menu, 9 cleanup (:130-311)
constexpr int kStickDiv = 237;         // 1650 (main :54..), the left stick divisor
constexpr int kSlamDiv = 504;          // 560 (main), the right-stick slam divisor
constexpr int kSlamMs = 503;           // 30 (main), slam duration
constexpr int kMouseHi = 238, kMouseLo = 239; // 512, 256 (main): mouse divisor by sensitivity
constexpr int kExtent = 455;           // table half extents vec3 {0.53, 0.992, 0.82}
constexpr int kAiPause = 486;          // AI pause, ms (:2027)
constexpr int kAiTune = 472;           // 0.0055 (main), the AI's acceleration
constexpr int kScore0 = 518, kScore1 = 519; // l_U517[0..1]: array header 517, elements after it
constexpr int kOpponent = 524, kGuard = 523;
constexpr int kHeading1 = 534;         // l_U532[1]: seat 1's heading (header 532)
constexpr int kAnchor1 = 540;          // l_U538[1]: seat 1's anchor object (header 538, :1615)
constexpr int kPaddle1Pos = 268, kPaddle1Radius = 274, kPaddle1Vel = 276; // l_U254 header 254, elements of 12 slots
constexpr int kPuckPos = 280, kPuckRadius = 286, kPuckVel = 288;            // l_U279
constexpr int kShift = -113;           // TBoGT

struct State {
  bool active = false;
  bool announced = false;
  std::uint8_t id = kNobody;
  std::string name;
  n::Ped puppet = 0, orig = 0;
  int s0 = 0, s1 = 0; // best scores seen (host, partner)
  // The slam state of the seat-0 helper (l_U497..l_U502), kept for the far seat.
  bool slam = false, latched = false;
  std::int32_t sx = 0, sy = 0;
  std::int32_t t_rest = 0, t_slam = 0;
  bool tried = false; // this game
  bool logged_drive = false;
} g;

float as_float(std::int32_t v) {
  float f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}
void put_float(std::int32_t& slot, float f) { std::memcpy(&slot, &f, sizeof(f)); }

// The decompiler prints some float statics as integers ("l_U237 = 1650"): read a static as a number either way (small
// magnitudes are integers; floats of any sensible size have large bit patterns).
float number(std::int32_t v) { return std::abs(v) < (1 << 20) ? static_cast<float>(v) : as_float(v); }

bool close_to(float a, float want, float tol) { return std::fabs(a - want) <= tol; }

bool readable_range(const void* p, std::size_t bytes) {
  const auto* a = static_cast<const std::uint8_t*>(p);
  const auto* end = a + bytes;
  while (a < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (!(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD)) return false;
    a = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  }
  return true;
}

bool ped_ok(n::Ped p) { return p && n::DoesCharExist(p) && !n::IsCharDead(p); }

int shift() { return episode::current() == episode::Id::TBoGT ? kShift : 0; }

// Fingerprints of main's init (:5-119) and the arrays' spacing: stick divisor 1650, slam divisor 560, mouse 512 / 256,
// table extents {0.53, 0.992}, AI tuning 0.0055; the paddles' radii equal and plausible, the puck's too.
bool layout_ok(const std::int32_t* s, int d) {
  const auto at = [&](int slot) { return s[slot + d]; };
  if (!close_to(number(at(kStickDiv)), 1650.f, 0.5f) || !close_to(number(at(kSlamDiv)), 560.f, 0.5f)) return false;
  if (!close_to(number(at(kMouseHi)), 512.f, 0.5f) || !close_to(number(at(kMouseLo)), 256.f, 0.5f)) return false;
  if (!close_to(as_float(at(kExtent)), 0.53f, 0.001f) || !close_to(as_float(at(kExtent + 1)), 0.992f, 0.001f)) return false;
  if (!close_to(as_float(at(kAiTune)), 0.0055f, 0.0001f)) return false;
  const float r0 = as_float(at(262)), r1 = as_float(at(kPaddle1Radius)), rp = as_float(at(kPuckRadius));
  if (!(r0 > 0.01f && r0 < 0.3f && close_to(r0, r1, 0.001f) && rp > 0.005f && rp < 0.3f)) return false;
  const int state = at(kState);
  return state >= 0 && state <= 10;
}

void seat_puppet(n::Ped puppet, n::Ped orig, n::Handle anchor, float heading) {
  n::SetCharCoordinatesNoOffset(puppet, n::GetCharCoordinates(orig));
  n::SetCharHeading(puppet, heading);
  n::FreezeCharPosition(puppet, true);
  n::SetCharCollision(puppet, false);
  n::SetBlockingOfNonTemporaryEvents(puppet, true);
  // The script's own call (:1648): ATTACH_PED_TO_OBJECT(ped, anchor, 0, 0, 0, 0, 0, 0, 1, 0).
  n::invoke(kAttachPedToObject, puppet, anchor, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1, 0);
  n::FreezeCharPosition(puppet, true);
  n::SetCharVisible(orig, false);
}

void free_puppet(n::Ped puppet) {
  if (!puppet || !n::DoesCharExist(puppet)) return;
  n::invoke(kDetachPed, puppet, 1);
  n::FreezeCharPosition(puppet, false);
  n::SetCharCollision(puppet, true);
}

void announce(const char* how) {
  if (g.announced) return;
  g.announced = true;
  if (g.s0 >= 7)
    phone::notify(std::format("Air hockey: you beat {} {}-{}.", g.name, g.s0, g.s1));
  else if (g.s1 >= 7)
    phone::notify(std::format("Air hockey: {} beat you {}-{}.", g.name, g.s1, g.s0));
  else
    return;
  log::info("air hockey: player {} ({}) {}-{} ({})", g.id, g.name, g.s0, g.s1, how);
}

void finish(std::int32_t* st, int d, const char* why) {
  if (!g.active) return;
  if (st) {
    if (st[kOpponent + d] == g.puppet) st[kOpponent + d] = ped_ok(g.orig) ? g.orig : 0;
    put_float(st[kAiPause + d], 0.0f);
  }
  if (g.orig && n::DoesCharExist(g.orig)) n::SetCharVisible(g.orig, true);
  free_puppet(g.puppet);
  activity_seats::unseat(g.id);
  log::info("air hockey: with player {} over ({}; {}-{})", g.id, why, g.s0, g.s1);
  const bool tried = g.tried;
  g = State{};
  g.tried = tried;
}

// ---- driving paddle 1 (sub_14423 :2115, mirrored)

// The seat-0 helper writes velocity (-ix/div, iy/div) unless it would push the paddle into a table wall. Seat 1 sits on
// the opposite side, so the same helper with both axes negated and paddle 1's slots.
void apply(std::int32_t* st, int d, int ix, int iy, float div) {
  ix = -ix, iy = -iy;
  const float px = as_float(st[kPaddle1Pos + d]), py = as_float(st[kPaddle1Pos + 1 + d]);
  const float r = as_float(st[kPaddle1Radius + d]);
  const float ex = as_float(st[kExtent + d]), ey = as_float(st[kExtent + 1 + d]);
  if ((ix < 0 || (px - r) > -ex) && (ix > 0 || (px + r) < ex)) put_float(st[kPaddle1Vel + d], -static_cast<float>(ix) / div);
  if ((iy > 0 || (py - r) > -ey) && (iy < 0 || (py + r) < ey)) put_float(st[kPaddle1Vel + 1 + d], static_cast<float>(iy) / div);
}

// sub_14980 (:2126) mirrored: the paddle must not squeeze the puck into the wall behind it.
void pinch(std::int32_t* st, int d) {
  const float px = as_float(st[kPaddle1Pos + d]), py = as_float(st[kPaddle1Pos + 1 + d]);
  const float r = as_float(st[kPaddle1Radius + d]);
  const float qx = as_float(st[kPuckPos + d]), qy = as_float(st[kPuckPos + 1 + d]), qr = as_float(st[kPuckRadius + d]);
  const float ex = as_float(st[kExtent + d]), ey = as_float(st[kExtent + 1 + d]);
  if (std::hypot(px - qx, py - qy) >= r + qr + 0.01f) return;
  float vx = as_float(st[kPaddle1Vel + d]), vy = as_float(st[kPaddle1Vel + 1 + d]);
  bool blocked = false;
  if (vy < 0.0f && (qy - qr) <= (-ey + 0.01f)) {
    blocked = true;
    vy = 0.0f;
  }
  if ((vx < 0.0f && (qx - qr) <= (-ex + 0.01f)) || (vx > 0.0f && (qx + qr) >= (ex - 0.01f))) {
    if (blocked) put_float(st[kPuckVel + 1 + d], as_float(st[kPuckVel + 1 + d]) + 0.01f);
    vx = 0.0f;
  }
  put_float(st[kPaddle1Vel + d], vx);
  put_float(st[kPaddle1Vel + 1 + d], vy);
}

void drive(std::int32_t* st, int d) {
  const auto s = activity_seats::sample(g.id);
  const int lx = s.fresh ? s.lx : 0, ly = s.fresh ? s.ly : 0, rx = s.fresh ? s.rx : 0, ry = s.fresh ? s.ry : 0;
  const auto now = static_cast<std::int32_t>(n::GetGameTimer());
  const float div = number(st[kStickDiv + d]), slam_div = number(st[kSlamDiv + d]);
  const std::int32_t slam_ms = static_cast<std::int32_t>(number(st[kSlamMs + d]));
  if (!g.slam) {
    apply(st, d, lx, ly, div);
    if (!g.latched && (rx * rx + ry * ry) > 12100 && (now - g.t_rest) < 150) {
      g.latched = g.slam = true;
      g.sx = rx, g.sy = ry;
      apply(st, d, g.sx, g.sy, slam_div);
      g.t_slam = now;
    }
    if (ry == 0 && rx == 0) {
      g.latched = false;
      g.t_rest = now;
    }
  } else {
    apply(st, d, g.sx, g.sy, slam_div);
    if (now - g.t_slam > slam_ms) g.slam = false;
  }
  if (ry == 0 && rx == 0 && ly == 0 && lx == 0) {
    const float sens = 0.5f; // the host's GET_MOUSE_SENSITIVITY is not the partner's; the middle of the range
    const float mouse_div = number(st[kMouseHi + d]) + (number(st[kMouseLo + d]) - number(st[kMouseHi + d])) * sens;
    apply(st, d, s.fresh ? s.mx : 0, s.fresh ? s.my : 0, mouse_div);
  }
  pinch(st, d);
  if (!g.logged_drive && (std::fabs(as_float(st[kPaddle1Vel + d])) > 0.0f || std::fabs(as_float(st[kPaddle1Vel + 1 + d])) > 0.0f)) {
    g.logged_drive = true;
    log::info("air hockey: paddle 1 moves with player {}'s input (velocity {:.4f} {:.4f})", g.id,
              as_float(st[kPaddle1Vel + d]), as_float(st[kPaddle1Vel + 1 + d]));
  }
}

void begin(const Frame& f, std::int32_t* st, int d) {
  const auto partners = partners_near(f);
  if (partners.empty()) return;
  const Player& who = *partners.front();
  const n::Ped orig = st[kOpponent + d];
  const n::Handle anchor = st[kAnchor1 + d];
  if (!ped_ok(orig) || orig == host_ped() || !anchor || !ped_ok(who.ped)) {
    log::warn("air hockey: not as expected (opponent {}, anchor {}, partner ped {}); left vanilla", orig, anchor, who.ped);
    return;
  }
  seat_puppet(who.ped, orig, anchor, as_float(st[kHeading1 + d]));
  activity_seats::seat(who.id, who.ped, "air_hockey", kPad);
  activity_seats::set_stand_in(who.id, orig);
  st[kOpponent + d] = who.ped;
  put_float(st[kAiPause + d], 1.0e9f);
  put_float(st[kPaddle1Vel + d], 0.0f);
  put_float(st[kPaddle1Vel + 1 + d], 0.0f);
  g = State{};
  g.active = g.tried = true;
  g.id = who.id, g.name = who.name, g.puppet = who.ped, g.orig = orig;
  phone::notify(std::format("Air hockey: {} takes the far side. Left stick / mouse moves, flick the right stick to slam.", who.name));
  log::info("air hockey: player {} ({}) seated opposite (ped {}, stand-in {}, guard {})", who.id, who.name, who.ped, orig,
            st[kGuard + d]);
}

void run(const Frame& f) {
  const auto ep = episode::current();
  if (ep != episode::Id::TLAD && ep != episode::Id::TBoGT) return;
  const int d = shift();
  std::int32_t* st = activity_seats::statics("air_hockey");
  if (!st || !readable_range(st, kWords * sizeof(std::int32_t))) {
    if (g.active) finish(nullptr, d, "script gone");
    g.tried = false;
    return;
  }
  const int state = st[kState + d];
  if (g.active) {
    if (state < 3 || state > 8) return finish(st, d, "game over");
    if (!activity_seats::seated(g.id) || !ped_ok(g.puppet)) return finish(st, d, "player left");
    if (st[kOpponent + d] != g.puppet) return finish(st, d, "script changed the opponent");
    put_float(st[kAiPause + d], 1.0e9f);
    g.s0 = std::max(g.s0, static_cast<int>(st[kScore0 + d]));
    g.s1 = std::max(g.s1, static_cast<int>(st[kScore1 + d]));
    if (state == 3) g.announced = false, g.s0 = g.s1 = 0; // a rematch: the script zeroes the scores
    if (state == 4 || state == 8) drive(st, d);
    if (state == 7) announce("game");
    return;
  }
  if (state < 3 || state > 8) {
    g.tried = false;
    return;
  }
  if (g.tried || state != 4) return;
  if (!layout_ok(st, d)) {
    g.tried = true;
    log::warn("air hockey: statics layout does not match the research (state {}); left vanilla", state);
    return;
  }
  g.tried = true;
  begin(f, st, d);
}

} // namespace

void register_hooks() {
  // No native hooks: the partner's input is read from activity_seats and written to the script's statics.
}

void host_tick(const Frame& f) { run(f); }

} // namespace ivc::activities::air_hockey
