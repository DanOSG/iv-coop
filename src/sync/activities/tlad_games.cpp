#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <random>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/episode.h"
#include "game/native_hooks.h"
#include "game/phone.h"
#include "sync/activities/activities.h"
#include "sync/activity_seats.h"

// TLAD's ambient table games, from the host's own scripts (docs/activities/tlad_minigames.md; decompiled scripts in
// research\GTAIVMissionScripts\TLAD, line numbers below are in arm_wrestling.txt / hi_lo_cards.txt).
//
//   Arm wrestling (1v1): the script has a second-human mode that was never switched on: static 116 = 1 makes seat B
//   read GET_POSITION_OF_ANALOGUE_STICKS(99, ...) through the same flick helper as seat A (pad 0). So a partner takes
//   seat B (their puppet becomes the opponent ped, l_U220), l_U116 = 1, and the seat is on pad 99 in activity_seats:
//   their stick is answered to those reads. The helper's mouse / keyboard fallbacks carry no pad, so a context flag
//   set by the pad-99 stick read makes the next GET_MOUSE_INPUT / GET_KEYBOARD_MOVE_INPUT / IS_USING_CONTROLLER the
//   partner's too (otherwise seat B would follow the host's mouse and keys).
//
//   Hi-lo cards (up to 4): seat 0 stays the host; partners take seats 1..3 (their puppets go into l_U253[k]). The
//   script plays those seats as AI: on their turn (l_U214 == 3, l_U215 == k) it decides after a pause from the card
//   alone and ends the turn through l_U214 = 4. While a partner is on turn the adapter holds the AI off and decides
//   from the partner's higher/lower (the same controls the script reads for the host: 93/130/79 and 94/131/78) by
//   writing what the AI would have written: guess l_U116, reaction code l_U221, previous rank l_U216, state l_U214 = 4.
//
// Stakes stay as the script has them: arm wrestling pays and takes the host's score only (entry fee, 2x on a win, the
// puppet's "money" is the stand-in's), hi-lo antes / pays through ADD_SCORE for the host and SET_CHAR_MONEY on the
// seat peds; a partner's winnings are announced to the host but not put in their wallet.
namespace ivc::activities::tlad_games {
namespace {
namespace n = natives;
using native_hooks::Call;

constexpr std::uint32_t kGetSticks = 0x4F7F4FAE, kGetMouse = 0x447B154B, kGetKeyboardMove = 0x4AF73456,
                        kIsUsingController = 0x669D053F,
                        kAttachPedToObject = 0x376917AB, kDetachPed = 0x2CD52C5C;

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

float as_float(std::int32_t v) {
  float f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}

bool near_float(std::int32_t v, float want) { return std::fabs(as_float(v) - want) < 0.0005f; }

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

// Puts `puppet` where the script put `orig`, pinned to the script's anchor object like every seat ped (the script's
// own ATTACH_PED_TO_OBJECT(ped, anchor, 0, 0,0,0, heading, 360, 0, 0)), and hides the original.
void seat_puppet(n::Ped puppet, n::Ped orig, n::Handle anchor, float heading, bool collision) {
  n::SetCharCoordinatesNoOffset(puppet, n::GetCharCoordinates(orig));
  n::SetCharHeading(puppet, heading);
  n::FreezeCharPosition(puppet, true);
  n::SetCharCollision(puppet, collision);
  n::SetBlockingOfNonTemporaryEvents(puppet, true);
  n::invoke(kAttachPedToObject, puppet, anchor, 0, 0.0f, 0.0f, 0.0f, heading, 360.0f, 0, 0);
  n::FreezeCharPosition(puppet, true);
  n::SetCharVisible(orig, false);
}

void free_puppet(n::Ped puppet) {
  if (!puppet || !n::DoesCharExist(puppet)) return;
  n::invoke(kDetachPed, puppet, 1);
  n::FreezeCharPosition(puppet, false);
  n::SetCharCollision(puppet, true);
}

// ================================================================== arm wrestling
// Statics (l_U<n> = statics[n]) and where they come from in arm_wrestling.txt:
//   184 main state (3 = match; case 3 at :1523, set at :1521 by the setup state), 186 match sub-state (0 at :1518, 4 =
//   flicking, :1755), 202 site, 207 seat B's anchor object (:3112), 220 the opponent ped (:1365), 116 second-human
//   flag (read :1730 and :1811, never written by the script), 130 seat B heading (:3113), 125 collision flag (:3153),
//   137 seat B power, 139 max power (20.0, main :25), 141 -15.0 (main :27), 199/200 rounds won (player / opponent).
// l_U289 is the highest slot the script names.
constexpr int kArmWords = 290;
constexpr int kAState = 184, kASub = 186, kASite = 202, kAAnchorB = 207, kAOpp = 220, kATwoHuman = 116, kAHeadingB = 130,
              kACollision = 125, kAPowerB = 137, kAMaxPower = 139, kADecay = 141, kARoundsA = 199, kARoundsB = 200;
constexpr int kArmMatch = 3, kArmFlicking = 4;
constexpr std::uint8_t kNobody = 0xFF;
constexpr int kPadB = 99; // the pad index the script reads seat B from (:1827, :1841)

struct Arm {
  bool active = false, tried = false, seat_b = false; // seat_b: the last stick read was seat B's (pad 99)
  std::uint8_t id = kNobody;
  std::string name;
  n::Ped puppet = 0, orig = 0;
  int rounds_a = 0, rounds_b = 0;
  DWORD flicking_since = 0;
  float max_power_b = 0;
  bool warned = false;
} g_arm;

bool arm_layout_ok(const std::int32_t* st) {
  // Fingerprints of the script's init (main): max power 20.0, decay -15.0; state and site in range.
  return near_float(st[kAMaxPower], 20.0f) && near_float(st[kADecay], -15.0f) && st[kAState] >= 0 && st[kAState] <= 4 &&
         st[kASite] >= 0 && st[kASite] <= 2;
}

void arm_finish(std::int32_t* st, const char* why) {
  if (!g_arm.active) return;
  if (st && st[kAOpp] == g_arm.puppet) st[kAOpp] = ped_ok(g_arm.orig) ? g_arm.orig : 0;
  if (st) st[kATwoHuman] = 0;
  if (g_arm.orig && n::DoesCharExist(g_arm.orig)) n::SetCharVisible(g_arm.orig, true);
  free_puppet(g_arm.puppet);
  activity_seats::unseat(g_arm.id);
  if (g_arm.rounds_a >= 2)
    phone::notify(std::format("Arm wrestling: you beat {}.", g_arm.name));
  else if (g_arm.rounds_b >= 2)
    phone::notify(std::format("Arm wrestling: {} beat you.", g_arm.name));
  log::info("TLAD games: arm wrestling with player {} over ({}; rounds {}-{})", g_arm.id, why, g_arm.rounds_a,
            g_arm.rounds_b);
  g_arm.active = false, g_arm.seat_b = false, g_arm.id = kNobody;
  g_arm.puppet = g_arm.orig = 0;
}

void arm_begin(const Frame& f, std::int32_t* st) {
  const auto partners = partners_near(f);
  if (partners.empty()) return; // nobody here: the script's own opponent, as ever
  const Player& who = *partners.front();
  const n::Ped orig = st[kAOpp];
  if (!arm_layout_ok(st) || st[kATwoHuman] != 0 || !st[kAAnchorB] || !ped_ok(orig) || !ped_ok(who.ped)) {
    log::warn("TLAD games: arm wrestling is not as expected (state {}, flag {}, anchor {}, opponent {}); left vanilla",
              st[kAState], st[kATwoHuman], st[kAAnchorB], orig);
    return;
  }
  seat_puppet(who.ped, orig, st[kAAnchorB], as_float(st[kAHeadingB]), st[kACollision] != 0);
  activity_seats::seat(who.id, who.ped, "arm_wrestling", kPadB);
  activity_seats::set_stand_in(who.id, orig);
  st[kAOpp] = who.ped;
  st[kATwoHuman] = 1;
  g_arm = {};
  g_arm.active = g_arm.tried = true;
  g_arm.id = who.id, g_arm.name = who.name, g_arm.puppet = who.ped, g_arm.orig = orig;
  phone::notify(std::format("Arm wrestling: {} takes the other seat. Flick the right stick left and right.", who.name));
  log::info("TLAD games: arm wrestling, player {} ({}) seated opposite (ped {}, stand-in {})", who.id, who.name, who.ped,
            orig);
}

void arm_tick(const Frame& f) {
  std::int32_t* st = activity_seats::statics("arm_wrestling");
  g_arm.seat_b = false;
  if (!st || !readable_range(st, kArmWords * sizeof(std::int32_t))) {
    if (g_arm.active) arm_finish(nullptr, "script gone");
    g_arm.tried = false;
    return;
  }
  const int state = st[kAState];
  if (g_arm.active) {
    if (state != kArmMatch) return arm_finish(st, "match over");
    if (!activity_seats::seated(g_arm.id) || !ped_ok(g_arm.puppet)) return arm_finish(st, "player left");
    g_arm.rounds_a = std::max(g_arm.rounds_a, static_cast<int>(st[kARoundsA]));
    g_arm.rounds_b = std::max(g_arm.rounds_b, static_cast<int>(st[kARoundsB]));
    // In-game check of the second-human mode: while flicking, their power (static 137) should move at least once.
    if (st[kASub] == kArmFlicking) {
      const DWORD now = GetTickCount();
      if (!g_arm.flicking_since) g_arm.flicking_since = now;
      g_arm.max_power_b = std::max(g_arm.max_power_b, as_float(st[kAPowerB]));
      if (!g_arm.warned && now - g_arm.flicking_since > 12000 && g_arm.max_power_b < 0.5f) {
        g_arm.warned = true;
        log::warn("TLAD games: arm wrestling seat B power stayed 0 for 12 s of flicking (partner idle, or the script's "
                  "AI block (:1848-1869) overrides pad-99 mode)");
      }
    }
    return;
  }
  if (state != kArmMatch) {
    g_arm.tried = false;
    return;
  }
  if (g_arm.tried || st[kASub] != 0) return; // seat once, in the first frames of a match
  g_arm.tried = true;
  arm_begin(f, st);
}

// Seat B's mouse / keyboard fallbacks (the helper at :4285-4340 reads GET_POSITION_OF_ANALOGUE_STICKS(pad), then,
// when the right stick is idle, GET_MOUSE_INPUT, then, when the left one is idle too and no controller is used,
// GET_KEYBOARD_MOVE_INPUT). The stick read for pad 99 flags the context; reads for any other pad end it.
bool arm_script(const Call& c) { return g_arm.active && lower(c.script) == "arm_wrestling"; }

void arm_on_sticks(const Call& c) {
  if (arm_script(c)) g_arm.seat_b = c.arg<std::int32_t>(0) == kPadB;
}

void arm_on_pair(const Call& c, bool mouse) {
  if (!g_arm.seat_b || !arm_script(c)) return;
  const auto s = activity_seats::sample(g_arm.id);
  if (auto* p = c.arg<std::int32_t*>(0)) *p = s.fresh ? (mouse ? s.mx : s.kx) : 0;
  if (auto* p = c.arg<std::int32_t*>(1)) *p = s.fresh ? (mouse ? s.my : s.ky) : 0;
}

void arm_on_using_controller(const Call& c) {
  if (!g_arm.seat_b || !arm_script(c)) return;
  const auto s = activity_seats::sample(g_arm.id);
  *reinterpret_cast<std::int32_t*>(c.ctx->result) = s.fresh && s.controller ? 1 : 0;
}

// ================================================================== hi-lo cards
// Statics in hi_lo_cards.txt (arrays have their count at the declared slot, elements after it: l_U253[k] is
// statics[254 + k]; checked against the declarations that follow: l_U253[4] -> l_U258, l_U224[4] -> l_U229,
// l_U133[4] -> l_U138, l_U327[4] of vec3 -> l_U340):
//   157 top state (4 = a player seated and playing; sub_7089(4) at the end of the join, :1086),
//   214 interactive state (0 ante, 1 wait, 3 AI turn, 4 AI decided, 5 player turn), 215 turn (0 = the host, 1..3),
//   159 deck index and l_U160[52] the deck (statics[161 + i], card%13+1 = rank, sub_17205 :4076),
//   116 guess (1 higher), 216 previous rank, 221 reaction code, 217 winner (:1951-1965), 253 seat peds,
//   224 anchors (statics[225 + k], :2250), 133 seat headings (statics[134 + k], :2259), 258..260 friend peds.
constexpr int kHiWords = 350;
constexpr int kHState = 157, kHInteractive = 214, kHTurn = 215, kHDeckIndex = 159, kHDeck = 160, kHGuess = 116,
              kHPrevRank = 216, kHReaction = 221, kHWinner = 217, kHPeds = 253, kHAnchors = 224, kHHeadings = 133,
              kHFriend0 = 258, kHCollisionFlag = 122, kHPad = 98;
constexpr int kHiPlaying = 4;
constexpr DWORD kTurnMs = 20000, kGraceMs = 700;

struct HiSeat {
  std::uint8_t id;
  std::string name;
  int slot; // 1..3
  n::Ped puppet, orig;
};
std::vector<HiSeat> g_hi;
bool g_hi_tried = false, g_hi_announced = false;

// The partner's turn being waited for.
struct Wait {
  bool on = false, chosen = false, timed_out = false, armed = false;
  int slot = 0;
  DWORD since = 0;
} g_wait;

std::mt19937& rng() {
  static std::mt19937 r{GetTickCount()};
  return r;
}
int pick(int a, int b) { return std::uniform_int_distribution<int>(0, 1)(rng()) ? a : b; }

bool hi_layout_ok(const std::int32_t* st) {
  // Fingerprints: ambient-table flag 1 (main :17), l_U141 0.03 (:19), state ranges.
  return st[kHCollisionFlag] == 1 && near_float(st[141], 0.03f) && st[kHState] >= 0 && st[kHState] <= 4 &&
         st[kHInteractive] >= 0 && st[kHInteractive] <= 18 && st[kHTurn] >= 0 && st[kHTurn] <= 3;
}

void hi_release(std::int32_t* st, HiSeat& s) {
  if (st && st[kHPeds + 1 + s.slot] == s.puppet) st[kHPeds + 1 + s.slot] = ped_ok(s.orig) ? s.orig : 0;
  if (s.orig && n::DoesCharExist(s.orig)) n::SetCharVisible(s.orig, true);
  free_puppet(s.puppet);
  activity_seats::unseat(s.id);
}

void hi_release_all(std::int32_t* st, const char* why) {
  if (g_hi.empty()) return;
  for (HiSeat& s : g_hi) hi_release(st, s);
  log::info("TLAD games: hi-lo cards, {} partner seat(s) released ({})", g_hi.size(), why);
  g_hi.clear();
  g_wait = {};
}

HiSeat* hi_seat_at(int slot) {
  for (HiSeat& s : g_hi)
    if (s.slot == slot) return &s;
  return nullptr;
}

void hi_begin(const Frame& f, std::int32_t* st) {
  const auto partners = partners_near(f);
  if (partners.empty()) return;
  std::string names;
  std::size_t next = 0;
  for (int slot = 1; slot <= 3 && next < partners.size(); ++slot) {
    const n::Ped orig = st[kHPeds + 1 + slot];
    const std::int32_t anchor = st[kHAnchors + 1 + slot];
    const bool friend_ped = orig == st[kHFriend0] || orig == st[kHFriend0 + 1] || orig == st[kHFriend0 + 2];
    if (!ped_ok(orig) || !anchor || friend_ped) continue; // a group friend's seat stays the script's
    const Player& who = *partners[next++];
    if (!ped_ok(who.ped)) continue;
    seat_puppet(who.ped, orig, anchor, as_float(st[kHHeadings + 1 + slot]), false);
    activity_seats::seat(who.id, who.ped, "hi_lo_cards", kHPad);
    activity_seats::set_stand_in(who.id, orig);
    st[kHPeds + 1 + slot] = who.ped;
    g_hi.push_back({who.id, who.name, slot, who.ped, orig});
    names += (names.empty() ? "" : ", ") + who.name;
    log::info("TLAD games: hi-lo cards, player {} ({}) seated in seat {} (ped {}, stand-in {})", who.id, who.name, slot,
              who.ped, orig);
  }
  if (!g_hi.empty()) phone::notify(std::format("Hi-lo cards: {} joined the table (higher / lower on their turn).", names));
}

// What the script's own player branch writes for a guess (:1504-1568 higher, :1558-1660 lower), so the seat's
// reaction animation fits the call: the code the AI/host branch picks by the rank of the card showing.
int reaction_code(bool higher, int rank) {
  if (higher) {
    switch (rank) {
      case 1: case 2: return 6;
      case 3: return pick(6, 4);
      case 4: case 5: return 4;
      case 6: case 7: case 8: return pick(4, 2);
      case 9: case 10: return 2;
      default: return 8;
    }
  }
  switch (rank) {
    case 12: case 13: return 7;
    case 11: return pick(7, 5);
    case 10: case 9: return 5;
    case 8: case 7: case 6: return pick(5, 3);
    case 5: case 4: return 3;
    default: return 9;
  }
}

void hi_decide(std::int32_t* st, bool higher) {
  const int index = st[kHDeckIndex];
  const int card = st[kHDeck + 1 + index];
  const int rank = card % 13 + 1;
  st[kHGuess] = higher ? 1 : 0;
  st[kHReaction] = reaction_code(higher, rank);
  st[kHPrevRank] = rank;
  st[kHInteractive] = 4; // what the AI branch ends with (:1481)
}

void hi_tick(const Frame& f) {
  std::int32_t* st = activity_seats::statics("hi_lo_cards");
  if (!st || !readable_range(st, kHiWords * sizeof(std::int32_t))) {
    hi_release_all(nullptr, "script gone");
    g_hi_tried = false;
    return;
  }
  if (st[kHState] != kHiPlaying) {
    hi_release_all(st, "game over");
    g_hi_tried = false, g_hi_announced = false;
    return;
  }
  if (!g_hi_tried) {
    g_hi_tried = true;
    if (!hi_layout_ok(st)) {
      log::warn("TLAD games: hi-lo cards' statics don't match the expected layout (state {}, interactive {}, turn {}); "
                "left vanilla", st[kHState], st[kHInteractive], st[kHTurn]);
    } else if (st[kHInteractive] <= 1) {
      hi_begin(f, st);
    } else {
      log::info("TLAD games: hi-lo cards already past the ante (state {}); no partner seated this game", st[kHInteractive]);
    }
  }
  if (g_hi.empty()) return;

  // Partners that left: their seat goes back to the script's ped.
  for (std::size_t i = 0; i < g_hi.size();) {
    HiSeat& s = g_hi[i];
    if (activity_seats::seated(s.id) && ped_ok(s.puppet)) {
      ++i;
      continue;
    }
    log::info("TLAD games: hi-lo cards, player {} left seat {}", s.id, s.slot);
    hi_release(st, s);
    g_hi.erase(g_hi.begin() + static_cast<std::ptrdiff_t>(i));
  }
  const int state = st[kHInteractive], turn = st[kHTurn];
  HiSeat* cur = hi_seat_at(turn);
  const DWORD now = GetTickCount();
  // Their buttons: higher = (2,93) / just (2,130) on keyboard, (2,79) on a pad; lower = (2,94) / (2,131) / (2,78),
  // the script's own tests for the host (:1558, :1612). The edges are drained every frame so an old press is stale.
  bool higher = false, lower_ = false;
  if (cur) {
    const bool pad = activity_seats::sample(cur->id).controller;
    const bool j130 = activity_seats::control(cur->id, 2, 130, true), j131 = activity_seats::control(cur->id, 2, 131, true);
    higher = pad ? activity_seats::control(cur->id, 2, 79) : (activity_seats::control(cur->id, 2, 93) || j130);
    lower_ = pad ? activity_seats::control(cur->id, 2, 78) : (activity_seats::control(cur->id, 2, 94) || j131);
  }
  if (!cur || (state != 3 && state != 4)) {
    g_wait.on = false;
  } else {
    if (!g_wait.on || g_wait.slot != turn) g_wait = {true, false, false, false, turn, now};
    if (!g_wait.chosen && !g_wait.timed_out) {
      if (state == 4) { // the script's AI decided first: hold it, the partner's call replaces it
        st[kHInteractive] = 3;
        st[kHReaction] = 0;
      }
      if (!higher && !lower_ && now - g_wait.since > kGraceMs) g_wait.armed = true; // released since their turn began
      if (g_wait.armed && higher != lower_ && st[kHDeckIndex] >= 0 && st[kHDeckIndex] < 52) {
        hi_decide(st, higher);
        g_wait.chosen = true;
        log::info("TLAD games: hi-lo cards, {} calls {}", cur->name, higher ? "higher" : "lower");
      } else if (now - g_wait.since > kTurnMs) {
        g_wait.timed_out = true;
        log::info("TLAD games: hi-lo cards, {} did not choose in {} s: the script decides", cur->name, kTurnMs / 1000);
        phone::notify(std::format("Hi-lo cards: {} took too long, the dealer plays their turn.", cur->name));
      }
    }
  }
  // The pot: announced for a partner (the script pays the host and the seat ped only).
  if (state == 15 && !g_hi_announced) {
    g_hi_announced = true;
    if (const HiSeat* w = hi_seat_at(st[kHWinner])) phone::notify(std::format("Hi-lo cards: {} wins the pot.", w->name));
  } else if (state != 15 && state != 16) {
    g_hi_announced = false;
  }
}

// The game ends through sub_793 (:2627), which starts with SET_PLAYER_CONTROL(player, 1) and then releases or re-seats
// whatever is in l_U253 as ambient table sitters: give the script its own peds back before that runs.
} // namespace

void register_hooks() {
  using native_hooks::on;
  on(kGetSticks, "GET_POSITION_OF_ANALOGUE_STICKS", arm_on_sticks);
  on(kGetMouse, "GET_MOUSE_INPUT", [](const Call& c) { arm_on_pair(c, true); });
  on(kGetKeyboardMove, "GET_KEYBOARD_MOVE_INPUT", [](const Call& c) { arm_on_pair(c, false); });
  on(kIsUsingController, "IS_USING_CONTROLLER", arm_on_using_controller);
  // No SET_PLAYER_CONTROL hook for hi-lo's close-down: hooking that native crashed the game (an engine worker, when a
  // player slot was reused on joining); the per-frame check releases the seats when the table closes instead.
}

void host_tick(const Frame& f) {
  if (episode::current() != episode::Id::TLAD) return;
  arm_tick(f);
  hi_tick(f);
}

} // namespace ivc::activities::tlad_games
