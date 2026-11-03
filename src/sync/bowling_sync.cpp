#include "sync/bowling_sync.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <map>

#include "core/log.h"
#include "game/episode.h"
#include "sync/activity_seats.h"
#include "game/native_hooks.h"
#include "game/script.h"
#include "net/bytes.h"
#include "net/protocol.h"

namespace ivc::bowling_sync {
namespace {
namespace n = natives;
using native_hooks::Call;

constexpr std::uint32_t kCreateObjectNoOffset = 0x75C51A26, kSetObjectInitialVelocity = 0x41ED206B,
                        kIsObjectUpright = 0x1EE13E29, kGetSticks = 0x4F7F4FAE, kGetMouse = 0x447B154B,
                        kIsControlPressed = 0x0E635761, kIsControlJustPressed = 0x4CB729F1,
                        kIsUsingController = 0x669D053F, kSetTextScale = 0x02C069E5, kSetTextColour = 0x19C967B5,
                        kSetTextDropshadow = 0x58F5023F, kSetTextFont = 0x75363BB5, kSetTextWrap = 0x19D006EB,
                        kSetTextProportional = 0x15585A65, kSetTextBackground = 0x768F5140,
                        kDisplayLiteral = 0x661B239A, kDrawRect = 0x3B2526E3, kSetPlayerControl = 0x1A6203EA;
// tenpinbowl's models (CREATE_OBJECT_NO_OFFSET): the pin and the ball.
constexpr std::int32_t kPinModel = -190707996, kBallModel = -1862626618;
// The script's own "pin is down" test (sub_65157): moved over 0.2 m on the ground, or tilted over 15 degrees.
constexpr float kPinMoved = 0.2f, kPinTilt = 15.0f;
constexpr DWORD kBurstMs = 700; // pins created closer together than this are one rack
constexpr DWORD kMinRollMs = 2500, kQuietMs = 1200, kMaxRollMs = 14000;
constexpr DWORD kInputMs = 33, kInputStaleMs = 500, kResendMs = 2000, kStaleMs = 6000, kOverShowMs = 10000;
constexpr int kFrames = 10;
constexpr std::uint8_t kFriendId = 0xFE; // the AI friend's card in the scorecard (not a session id)

enum Op : std::uint8_t { OpState = 0, OpInput = 1 };

// The script's input reads that a partner answers for: (pad, control). Index = bit in the input masks.
struct Ctl {
  int pad, control;
};
constexpr Ctl kCtl[] = {{2, 4}, {2, 77}, {0, 77}, {2, 87}, {2, 78}, {0, 78}, {2, 3}};
constexpr int kNumCtl = static_cast<int>(sizeof(kCtl) / sizeof(kCtl[0]));

int ctl_index(int pad, int control) {
  for (int i = 0; i < kNumCtl; ++i)
    if (kCtl[i].pad == pad && kCtl[i].control == control) return i;
  return -1;
}

struct Card {
  std::uint8_t id = 0;
  std::string name;
  std::vector<Roll> frames;
};

// The game as everyone sees it (host: the engine's state, sent on change; others: the last received).
struct View {
  bool active = false, over = false;
  std::uint8_t cur_id = 0; // whose turn
  std::uint8_t frame = 0;  // 0-based frame being bowled (of kFrames, shared by all players)
  std::vector<Card> cards;
};

struct Input {
  std::int32_t lx = 0, ly = 0, rx = 0, ry = 0, mx = 0, my = 0;
  std::uint8_t pressed = 0, just = 0;
  bool controller = false;
};

struct Pin {
  n::Object h = 0;
  n::Vector3 start{}; // rack position, taken at the release
  n::Vector3 last{};
  bool down = false;
};

bool g_hosting = false;
std::uint8_t g_my_id = 0;
View g_view; // host: authoritative; others: the last received

// Host engine.
bool g_locked_solo = false;       // a ball was thrown with nobody else near: vanilla for the rest of the script
std::uint8_t g_ball_in_frame = 0; // balls thrown so far in the current frame
DWORD g_last_eval = 0;
std::vector<Pin> g_pins;
n::Object g_ball = 0;
DWORD g_last_pin_at = 0;
enum class Phase { Aim, Rolling } g_phase = Phase::Aim;
DWORD g_release_at = 0, g_last_move_at = 0;
int g_standing_before = 0;
bool g_dirty = false;
std::uint8_t g_seq = 0;
DWORD g_last_send = 0, g_over_at = 0;
Input g_in;                 // the controlling partner's latest input
std::uint8_t g_in_just = 0; // latched just-pressed bits, cleared as the script reads them
DWORD g_in_at = 0;

// Host: driving the script's own card (see "the script's card" below).
bool g_script_mode = false;
std::uint8_t g_slot0_owner = 0;                              // whose card the script's slot 0 holds now
std::map<std::uint8_t, std::vector<std::int32_t>> g_snaps;   // each human's card while another bowler's is in slot 0
std::int32_t g_empty = -99;                                  // the script's "no roll yet" value, as seen in memory
int g_seen_cnt[2] = {0, 0}, g_seen_frame[2] = {0, 0};        // per script card: rolls recorded, frame index
bool g_friend = false;                                       // l_U263 == 2: card 1 is the AI friend
bool g_friend_virtual = false, g_slot0_done_virtual = false; // our overrides that steer the script's own rotation
std::int32_t g_friend_real_frame = 0;
bool g_proxy = false;                                        // the controlling partner left: the host finishes the frame
const std::int32_t* g_checked_statics = nullptr;
// Each partner bowls as their own character: on their turn card 0's bowler (word 0, the ped the script runs up,
// hands the ball and animates) is their puppet, seated through sync/activity_seats (which also makes the script's
// GET_PLAYER_CHAR return it). The host's own ped goes back for the host's and the friend's turns.
std::int32_t g_host_card_ped = 0;
std::int32_t* g_card_ped_slot = nullptr; // card 0's word 0 in the statics it was taken from
std::uint8_t g_bowler_seat = 0xFF; // the partner seated as card 0's bowler, if any

// Partner.
bool g_froze = false;       // we switched our player's control off for our turn
std::uint8_t g_rx_seq = 0;
bool g_rx_valid = false;
DWORD g_rx_at = 0;
DWORD g_sent_at = 0;
std::uint8_t g_pending_just = 0; // just-presses since the last packet that went out

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool from_bowling(const Call& c) { return lower(c.script) == "tenpinbowl"; }

n::Ped player_ped() {
  const n::Ped p = n::GetPlayerChar(n::GetPlayerId());
  return p && n::DoesCharExist(p) ? p : 0;
}

// ---- scoring helpers

int pins_of(const Roll& f) {
  int s = 0;
  for (int i = 0; i < f.n; ++i) s += f.r[i];
  return s;
}

// The rolls after frame `i`, in order (as many as exist).
std::vector<int> later_rolls(const std::vector<Roll>& fr, std::size_t i) {
  std::vector<int> out;
  for (std::size_t k = i + 1; k < fr.size(); ++k)
    for (int j = 0; j < fr[k].n; ++j) out.push_back(fr[k].r[j]);
  return out;
}

// "X", "7/", "9-" style marks for one frame.
std::string frame_marks(const Roll& f) {
  std::string s;
  int standing = 10;
  for (int i = 0; i < f.n; ++i) {
    const int p = f.r[i];
    if (p >= standing) s += standing == 10 ? 'X' : '/', standing = 10;
    else s += p == 0 ? std::string("-") : std::to_string(p), standing -= p;
  }
  return s;
}

// ---- engine (host)

void restore_bowler() {
  if (g_bowler_seat != 0xFF) activity_seats::unseat(g_bowler_seat);
  g_bowler_seat = 0xFF;
  activity_seats::set_active(0xFF);
  if (g_host_card_ped && g_card_ped_slot && g_checked_statics &&
      activity_seats::statics("tenpinbowl") == g_checked_statics)
    *g_card_ped_slot = g_host_card_ped;
  g_host_card_ped = 0, g_card_ped_slot = nullptr;
}

void reset_game() {
  restore_bowler();
  g_view = View{};
  g_ball_in_frame = 0;
  g_pins.clear();
  g_phase = Phase::Aim;
  g_in = {}, g_in_just = 0;
  g_script_mode = g_friend = g_friend_virtual = g_slot0_done_virtual = g_proxy = false;
  g_snaps.clear();
  g_checked_statics = nullptr;
}

Card* card_of(std::uint8_t id) {
  for (Card& c : g_view.cards)
    if (c.id == id) return &c;
  return nullptr;
}

bool in_session(const Frame& f, std::uint8_t id) {
  return id == g_my_id || id == kFriendId || std::any_of(f.players.begin(), f.players.end(), [&](const Player& p) { return p.id == id; });
}

// Next player in the rotation that is still in the session.
void advance_player(const Frame& f) {
  const auto& cs = g_view.cards;
  std::size_t at = 0;
  for (std::size_t i = 0; i < cs.size(); ++i)
    if (cs[i].id == g_view.cur_id) at = i;
  for (std::size_t step = 1; step <= cs.size(); ++step) {
    const Card& c = cs[(at + step) % cs.size()];
    if (in_session(f, c.id)) {
      g_view.cur_id = c.id;
      return;
    }
  }
}

// One ball bowled by the current player with `pins` knocked down.
void apply_ball(const Frame& f, int pins) {
  Card* c = card_of(g_view.cur_id);
  if (!c) return;
  const bool last = g_view.frame == kFrames - 1;
  if (g_ball_in_frame == 0) c->frames.push_back(Roll{{0, 0, 0}, 0, static_cast<std::uint8_t>(last ? 1 : 0)});
  Roll& r = c->frames.back();
  r.r[r.n++] = static_cast<std::uint8_t>(std::clamp(pins, 0, 10));
  ++g_ball_in_frame;
  bool done;
  if (!last) {
    done = r.r[0] == 10 || r.n >= 2;
  } else if (r.n == 1) {
    done = false; // the 10th frame always has a second ball; a strike or spare earns the third
  } else if (r.n == 2) {
    done = !(r.r[0] == 10 || r.r[0] + r.r[1] == 10);
  } else {
    done = true;
  }
  log::info("Bowling: {} bowled {} (frame {}, ball {}){}", c->name, pins, g_view.frame + 1, g_ball_in_frame,
            done ? ", frame over" : "");
  if (done) {
    g_ball_in_frame = 0;
    if (g_view.frame + 1 >= kFrames) {
      g_view.over = true;
      g_over_at = GetTickCount();
    } else {
      ++g_view.frame;
      advance_player(f);
    }
  }
  g_dirty = true;
}

void start_game(const Frame& f, const std::vector<const Player*>& mates) {
  reset_game();
  g_view.active = true;
  std::string who;
  for (const Player& p : f.players)
    if (p.id == g_my_id) g_view.cards.push_back({p.id, p.name, {}});
  for (const Player* p : mates) g_view.cards.push_back({p->id, p->name, {}}), who += " " + p->name;
  g_view.cur_id = g_my_id;
  g_dirty = true;
  log::info("Bowling: game started, host plus{}", who);
}

// ---- the script's card (host)
//
// tenpinbowl's scorecard (IV slot numbers; TLAD/TBoGT in Layout below) is l_U617[player] (one 113-word element per bowler, element i at static 618 + 113 i,
// the array's size word at 617): _fU0 the ped, _fU4[21][3] the rolls ({pins, frame, kind}; -99 = unused,
// 999 = unused kind), _fU344[10] frame scores, _fU388 the frame being bowled (0..9), _fU396 the strike run,
// _fU400[10] and _fU444 "finished". Nested arrays carry a size word each, so the layout is checkable: every
// size word must match before we touch anything. The script always bowls card 0 as the player and card 1 as
// an AI friend (l_U263 = 2); its own sub_13200 picks the turn between them from the cards' frame counters.
//
// Method: the script's card 0 is a window onto whichever *human* is bowling. When the script finishes a
// frame (a roll was recorded and _fU388 moved on, or _fU444 was set), we save card 0's words for that human
// and write the next bowler's saved card (or a fresh one) over them, before the script's next
// sub_13200/frame start. The script then plays a full 10 frames for each human and ends itself when card 0
// (and card 1) are finished. With a friend we steer sub_13200 by faking the two fields it reads: card 1's
// _fU388 reads 9 while a human is up (nothing is "ahead" of it), card 0's _fU444 reads 1 while the friend is up.
// Card struct (per element, same in all three games' scripts): see the field offsets below. Only the slot numbers of
// the arrays/variables differ per episode; they are in Layout, picked by script_statics().
constexpr int kCardWords = 113;
constexpr int kWRolls = 1, kWRoll0 = 2, kWScores = 86, kWFrame = 97, kWMarks = 100, kWFinished = 111;
constexpr int kNumRolls = 21;
constexpr std::int32_t kEmptyOld = 65437; // how the decompile prints -99

// Static slots of each game's tenpinbowl, matched by the same code in the three decompiles (research/
// GTAIVMissionScripts/{IV,TLAD,TBOGT}/tenpinbowl.txt). The slots are not a uniform shift between games, and the card
// struct (113 words, offsets above) is identical in all three (the same _fU0.._fU448 set, the same init code).
//  - players/turn: IV `l_U263 = l_U1366 + l_U1367;` (:683) and `l_U264 = 0;` (:46, :299); TLAD l_U235 (:721, capped
//    at 6 :724) / l_U236 (:34, :289); TBOGT l_U25 (:665, capped at 2 :668: the card array is 2 long) / l_U26 (:5, :260).
//  - cards: sub_11987 in IV (:1236, `l_U617[iVar2]._fU4[I][2] = 999`), the same function at TLAD :1277 (l_U589) and
//    TBOGT :1225 (l_U379). The array's length shows in where the next static starts: the ped l_U1296 (IV, 617 + 1 +
//    6*113 = 1296), l_U1268 (TLAD, 589 + 679), l_U606 (TBOGT, 379 + 1 + 2*113) - so TBOGT's card array has 2 elements.
//  - state: IV l_U2146 is set to 28 in the init block (:219) next to 7 (l_U2147) and 3 (l_U2149), and to 1 at :298/:4581
//    with the turn reset; TLAD l_U2134 (:207, :288), TBOGT l_U1068 (:178, :259) have identical init/neighbours and the
//    same 56 uses with the same assigned-value histogram (27 x6, 17 x5, 33 x4, 22 x4).
//  - statics_words: highest slot the script references + 1 (IV keeps 2200, its original margin); readable_range checks it.
struct Layout {
  int players, turn, cards, state, statics_words, max_cards;
};
constexpr Layout kLayoutIV{263, 264, 617, 2146, 2200, 6};
constexpr Layout kLayoutTLAD{235, 236, 589, 2134, 2141, 6};
constexpr Layout kLayoutTBoGT{25, 26, 379, 1068, 1075, 2};
Layout g_lay{};

// Fills g_lay for the running game; nullptr (shared frames, no script card swap) while the episode is unknown.
std::int32_t* script_statics() {
  switch (episode::current()) {
    case episode::Id::IV: g_lay = kLayoutIV; break;
    case episode::Id::TLAD: g_lay = kLayoutTLAD; break;
    case episode::Id::TBoGT: g_lay = kLayoutTBoGT; break;
    default: return nullptr;
  }
  auto* t = static_cast<std::uint8_t*>(script::find_thread("tenpinbowl"));
  return t ? *reinterpret_cast<std::int32_t**>(t + 0x58) : nullptr;
}

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

std::int32_t* card_at(std::int32_t* st, int i) { return st + g_lay.cards + 1 + kCardWords * i; }
bool is_empty(std::int32_t v) { return v == -99 || v == kEmptyOld; }
std::int32_t* roll_at(std::int32_t* c, int i) { return c + kWRoll0 + 4 * i + 1; } // -> {pins, frame, kind}

int roll_count(std::int32_t* c) {
  int k = 0;
  while (k < kNumRolls && !is_empty(roll_at(c, k)[0])) ++k;
  return k;
}

bool card_ok(std::int32_t* c, std::string& why) {
  if (c[kWRolls] != kNumRolls || c[kWScores] != 10 || c[kWMarks] != 10) return why = "array size words", false;
  for (int i = 0; i < kNumRolls; ++i)
    if (c[kWRoll0 + 4 * i] != 3) return why = "roll entry size word", false;
  if (c[kWFrame] < 0 || c[kWFrame] > 9) return why = "frame index", false;
  if (c[kWFinished] != 0 && c[kWFinished] != 1) return why = "finished flag", false;
  bool gap = false;
  for (int i = 0; i < kNumRolls; ++i) {
    const std::int32_t* r = roll_at(c, i);
    if (is_empty(r[0])) {
      gap = true;
      continue;
    }
    if (gap || r[0] < 0 || r[0] > 10 || r[1] < 0 || r[1] > 9 || r[2] < 0 || r[2] > 4) return why = "roll values", false;
  }
  return true;
}

// The statics look like tenpinbowl's: size words, counts and ranges. `players` out: IV l_U263.
bool script_ok(std::int32_t* st, int& players, std::string& why) {
  if (st[g_lay.cards] != g_lay.max_cards) return why = "card array size", false;
  players = st[g_lay.players];
  if (players < 1 || players > g_lay.max_cards) return why = "player count", false;
  if (st[g_lay.turn] < 0 || st[g_lay.turn] >= players) return why = "turn index", false;
  if (st[g_lay.state] < 0 || st[g_lay.state] > 80) return why = "state variable", false;
  for (int i = 0; i < players && i < 2; ++i)
    if (!card_ok(card_at(st, i), why)) return why = std::format("card {}: {}", i, why), false;
  return true;
}

std::vector<Roll> read_rolls(std::int32_t* c) {
  std::vector<Roll> out;
  std::int32_t cur = -1;
  for (int i = 0; i < kNumRolls; ++i) {
    const std::int32_t* r = roll_at(c, i);
    if (is_empty(r[0])) break;
    if (r[1] != cur) {
      out.push_back(Roll{{0, 0, 0}, 0, static_cast<std::uint8_t>(r[1] == kFrames - 1 ? 1 : 0)});
      cur = r[1];
    }
    Roll& f = out.back();
    if (f.n < 3) f.r[f.n++] = static_cast<std::uint8_t>(r[0]);
  }
  return out;
}

bool frame_complete(const Roll& r) {
  if (!r.last) return r.r[0] == 10 || r.n >= 2;
  return r.n >= 3 || (r.n == 2 && !(r.r[0] == 10 || r.r[0] + r.r[1] == 10));
}

bool finished(const Card& c) {
  return c.frames.size() >= static_cast<std::size_t>(kFrames) && frame_complete(c.frames.back());
}

std::vector<std::int32_t> save_card(std::int32_t* c) { return std::vector<std::int32_t>(c, c + kCardWords); }

// Words 1.. only: word 0 is the ped the script bowls with.
void load_card(std::int32_t* c, const std::vector<std::int32_t>& w) {
  for (int i = 1; i < kCardWords; ++i) c[i] = w[i];
}

// A new game's card for card 0 (sub_11987), cloned from the live one so the size words carry over.
std::vector<std::int32_t> fresh_card(std::int32_t* like) {
  std::vector<std::int32_t> w = save_card(like);
  for (int i = 0; i < kNumRolls; ++i) {
    w[kWRoll0 + 4 * i + 1] = g_empty, w[kWRoll0 + 4 * i + 2] = g_empty, w[kWRoll0 + 4 * i + 3] = 999;
  }
  for (int j = 0; j < 10; ++j) w[kWScores + 1 + j] = g_empty, w[kWMarks + 1 + j] = j == 0 ? 1 : 0;
  w[kWFrame] = w[kWFrame + 1] = w[kWFrame + 2] = 0; // _fU388, _fU392, _fU396
  w[kWFinished] = w[kWFinished + 1] = 0;            // _fU444, _fU448
  return w;
}

std::string friend_name(n::Ped ped) {
  if (!ped || !n::DoesCharExist(ped)) return "Friend";
  static const struct {
    const char* model;
    const char* name;
  } kFriends[] = {{"IG_ROMAN", "Roman"},       {"IG_JACOB", "Jacob"},     {"IG_LITTLE_JACOB", "Jacob"},
                  {"IG_BRUCIE", "Brucie"},     {"IG_PACKIE", "Packie"},   {"IG_PACKIE_MCREARY", "Packie"},
                  {"IG_DWAYNE", "Dwayne"},     {"IG_MANNY", "Manny"},     {"IG_MALLORIE", "Mallorie"},
                  {"IG_MICHELLE", "Michelle"}, {"IG_KATE", "Kate"},       {"IG_VLAD", "Vlad"},
                  {"IG_FRANCIS", "Francis"},   {"IG_ANTHONY", "Anthony"}, {"IG_ELIZABETA", "Elizabeta"}};
  const std::uint32_t m = n::GetCharModel(ped);
  for (const auto& e : kFriends)
    if (n::GetHashKey(e.model) == m) return e.name;
  return "Friend";
}

// Next bowler after `from` (cyclic, `from` last) who is still in the session and has frames left.
const Card* next_bowler(const Frame& f, std::uint8_t from) {
  const auto& cs = g_view.cards;
  std::size_t at = 0;
  for (std::size_t i = 0; i < cs.size(); ++i)
    if (cs[i].id == from) at = i;
  for (std::size_t step = 1; step <= cs.size(); ++step) {
    const Card& c = cs[(at + step) % cs.size()];
    if ((c.id == kFriendId || in_session(f, c.id)) && !finished(c)) return &c;
  }
  return nullptr;
}

void set_friend_virtual(std::int32_t* st, bool on) {
  if (!g_friend) return;
  std::int32_t* c1 = card_at(st, 1);
  if (on && !g_friend_virtual) {
    g_friend_real_frame = c1[kWFrame];
    c1[kWFrame] = 9; // nothing is ahead of a friend at the last frame: sub_13200 keeps/gives the turn to card 0
    g_friend_virtual = true;
  } else if (!on && g_friend_virtual) {
    c1[kWFrame] = g_friend_real_frame;
    g_seen_frame[1] = g_friend_real_frame;
    g_friend_virtual = false;
  }
}

void abandon_script_mode(const char* why) {
  if (!g_script_mode) return;
  restore_bowler();
  log::warn("Bowling: leaving the script-card mode ({}); the rest is scored from the pins, frames shared", why);
  g_script_mode = false;
  g_proxy = false;
  g_snaps.clear();
  std::erase_if(g_view.cards, [](const Card& c) { return c.id == kFriendId; });
  if (!card_of(g_view.cur_id)) g_view.cur_id = g_my_id;
  std::size_t frames = 0;
  for (const Card& c : g_view.cards) frames = std::max(frames, c.frames.size());
  g_view.frame = static_cast<std::uint8_t>(std::min<std::size_t>(frames, kFrames - 1));
  const Card* c = card_of(g_view.cur_id);
  g_ball_in_frame = c && !c->frames.empty() && !frame_complete(c->frames.back()) ? c->frames.back().n : 0;
  g_dirty = true;
}

// First throw with partners present: check the layout and start steering. Any doubt: stay on the pin scoring.
void begin_script_mode() {
  g_script_mode = false;
  std::int32_t* st = script_statics();
  if (!st || !readable_range(st, g_lay.statics_words * sizeof(std::int32_t))) {
    log::warn("Bowling: tenpinbowl's statics are not readable; frames shared");
    return;
  }
  int players = 0;
  std::string why;
  if (!script_ok(st, players, why)) {
    log::warn("Bowling: tenpinbowl's statics don't match the expected layout ({}); frames shared", why);
    return;
  }
  if (players > 2) {
    log::warn("Bowling: {} bowlers in the script (more than one friend); frames shared", players);
    return;
  }
  std::int32_t* c0 = card_at(st, 0);
  std::int32_t* c1 = players == 2 ? card_at(st, 1) : nullptr;
  if (roll_count(c0) != 0 || (c1 && roll_count(c1) != 0) || c0[kWFrame] != 0) {
    log::warn("Bowling: the script's card is already in play ({} rolls); frames shared", roll_count(c0));
    return;
  }
  g_empty = roll_at(c0, 0)[0]; // the "unused" value as the script stores it
  g_host_card_ped = c0[0], g_card_ped_slot = &c0[0];
  g_checked_statics = st;
  g_snaps.clear();
  g_slot0_owner = g_my_id;
  g_friend = c1 != nullptr;
  g_friend_virtual = g_slot0_done_virtual = false;
  g_proxy = false;
  if (g_friend) {
    g_view.cards.push_back({kFriendId, friend_name(static_cast<n::Ped>(c1[0])), {}});
    g_friend_real_frame = c1[kWFrame];
  }
  g_seen_cnt[0] = g_seen_cnt[1] = 0;
  g_seen_frame[0] = c0[kWFrame];
  g_seen_frame[1] = c1 ? c1[kWFrame] : 0;
  g_script_mode = true;
  log::info("Bowling: driving the script's scorecard ({} bowler(s) in the script{}), a full 10 frames each", players,
            g_friend ? ", one is a friend" : "");
}

// A roll was recorded in script card `k` (0: the human in slot 0, 1: the AI friend).
void roll_processed(const Frame& f, std::int32_t* st, int k, DWORD now) {
  std::int32_t* ck = card_at(st, k);
  Card* who = card_of(k == 0 ? g_slot0_owner : kFriendId);
  if (!who) return abandon_script_mode("no card for the roller");
  who->frames = read_rolls(ck);
  g_seen_cnt[k] = roll_count(ck);
  const bool over_frame = ck[kWFinished] != 0 || ck[kWFrame] != g_seen_frame[k];
  g_seen_frame[k] = ck[kWFrame];
  const bool mine = !who->frames.empty() && frame_complete(who->frames.back());
  log::info("Bowling: {} rolled ({} rolls, frame {}, turn state {}){}", who->name, g_seen_cnt[k], ck[kWFrame] + 1,
            st[g_lay.state], over_frame ? ", frame over" : "");
  if (mine != over_frame)
    log::warn("Bowling: the script's frame end ({}) and the mod's rules ({}) disagree for {}", over_frame, mine,
              who->name);
  g_dirty = true;
  if (!over_frame) {
    g_view.cur_id = who->id;
    g_view.frame = static_cast<std::uint8_t>(std::min<std::size_t>(who->frames.size() - (mine ? 0 : 1), kFrames - 1));
    return;
  }
  g_proxy = false;
  if (k == 0) g_snaps[g_slot0_owner] = save_card(ck); // before any of our overrides
  const Card* nb = next_bowler(f, who->id);
  if (!nb) {
    g_view.over = true;
    g_over_at = now;
    log::info("Bowling: game over");
    return;
  }
  if (nb->id == kFriendId) {
    // The friend is up: card 0 reads "finished" so sub_13200 moves on to card 1.
    set_friend_virtual(st, false);
    card_at(st, 0)[kWFinished] = 1;
    g_slot0_done_virtual = true;
  } else {
    if (nb->id != g_slot0_owner || g_slot0_done_virtual) {
      std::int32_t* c0 = card_at(st, 0);
      const auto it = g_snaps.find(nb->id);
      load_card(c0, it != g_snaps.end() ? it->second : fresh_card(c0));
      g_slot0_done_virtual = false;
      g_slot0_owner = nb->id;
      g_seen_cnt[0] = roll_count(c0);
      g_seen_frame[0] = c0[kWFrame];
    }
    set_friend_virtual(st, true);
  }
  g_view.cur_id = nb->id;
  g_view.frame = static_cast<std::uint8_t>(std::min<std::size_t>(nb->frames.size(), kFrames - 1));
  g_in = {}, g_in_just = 0;
  log::info("Bowling: next up {} (frame {})", nb->name, g_view.frame + 1);
}

// Card 0's bowler follows whose card it holds: a partner's turn puts their puppet in, anyone else's the host's ped.
void apply_bowler(const Frame& f, std::int32_t* st) {
  const std::uint8_t want = g_slot0_owner != g_my_id && g_view.cur_id == g_slot0_owner ? g_slot0_owner : 0xFF;
  if (want == g_bowler_seat) return;
  if (g_bowler_seat != 0xFF) activity_seats::unseat(g_bowler_seat);
  g_bowler_seat = 0xFF;
  n::Ped ped = 0;
  if (want != 0xFF)
    for (const Player& p : f.players)
      if (p.id == want) ped = p.ped;
  if (ped && n::DoesCharExist(ped)) {
    activity_seats::seat(want, ped, "tenpinbowl", -1);
    activity_seats::set_active(want);
    card_at(st, 0)[0] = static_cast<std::int32_t>(ped);
    g_bowler_seat = want;
    log::info("Bowling: {} bowls as their own character (ped {})", card_of(want) ? card_of(want)->name : "?", ped);
  } else {
    activity_seats::set_active(0xFF);
    if (g_host_card_ped) card_at(st, 0)[0] = g_host_card_ped;
  }
}

void script_tick(const Frame& f, DWORD now) {
  std::int32_t* st = script_statics();
  if (!st) return;
  if (st != g_checked_statics) {
    if (!readable_range(st, g_lay.statics_words * sizeof(std::int32_t))) return abandon_script_mode("statics moved");
    g_checked_statics = st;
  }
  int players = 0;
  std::string why;
  if (!script_ok(st, players, why)) return abandon_script_mode(why.c_str());
  if (players != (g_friend ? 2 : 1)) return abandon_script_mode("player count changed");
  // The controlling partner left in the middle of a frame: the host bowls it out on their behalf.
  if (!g_proxy && g_view.cur_id != g_my_id && g_view.cur_id != kFriendId && !in_session(f, g_view.cur_id)) {
    g_proxy = true;
    log::info("Bowling: player {} left mid-frame, the host finishes it", g_view.cur_id);
  }
  if (g_proxy) restore_bowler();
  else apply_bowler(f, st);
  const int n0 = roll_count(card_at(st, 0)), n1 = g_friend ? roll_count(card_at(st, 1)) : 0;
  if (n0 < g_seen_cnt[0] || n1 < g_seen_cnt[1]) return abandon_script_mode("the script's card was reset");
  if (n0 > g_seen_cnt[0]) return roll_processed(f, st, 0, now);
  if (g_friend && n1 > g_seen_cnt[1]) return roll_processed(f, st, 1, now);
}

void finish_roll(const Frame& f) {
  int down = 0;
  for (const Pin& p : g_pins)
    if (p.down) ++down;
  g_phase = Phase::Aim;
  g_pins.clear(); // a rack is only valid for the throw it was taken for
  if (g_script_mode) return; // the script's own card says what was bowled
  apply_ball(f, std::min(down, g_standing_before));
}

void track_roll(const Frame& f, DWORD now) {
  float moved = 0;
  for (Pin& p : g_pins) {
    if (!p.h || !n::DoesObjectExist(p.h)) continue; // the script swept it: keep what we last saw
    const n::Vector3 v = n::GetObjectCoordinates(p.h);
    moved += std::fabs(v.x - p.last.x) + std::fabs(v.y - p.last.y) + std::fabs(v.z - p.last.z);
    p.last = v;
    const float dx = v.x - p.start.x, dy = v.y - p.start.y;
    p.down = std::sqrt(dx * dx + dy * dy) > kPinMoved || !n::invoke<bool>(kIsObjectUpright, p.h, kPinTilt);
  }
  if (moved > 0.01f) g_last_move_at = now;
  const DWORD since = now - g_release_at;
  if (since >= kMaxRollMs || (since >= kMinRollMs && now - g_last_move_at >= kQuietMs)) finish_roll(f);
}

// Players near the host who join its game.
std::vector<const Player*> partners_in_range(const Frame& f) {
  std::vector<const Player*> out;
  const n::Ped me = player_ped();
  if (!me) return out;
  const n::Vector3 a = n::GetCharCoordinates(me);
  for (const Player& p : f.players) {
    if (p.id == g_my_id || !p.ped || !n::DoesCharExist(p.ped)) continue;
    const n::Vector3 b = n::GetCharCoordinates(p.ped);
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    if (std::sqrt(dx * dx + dy * dy + dz * dz) <= kPresentRange) out.push_back(&p);
  }
  return out;
}

void send_state(net::Session& net) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(std::uint8_t{OpState}).put(++g_seq)
      .put(static_cast<std::uint8_t>(g_view.active ? 1 : 0)).put(static_cast<std::uint8_t>(g_view.over ? 1 : 0))
      .put(g_view.cur_id).put(g_view.frame).put(static_cast<std::uint8_t>(g_view.cards.size()));
  for (const Card& c : g_view.cards) {
    w.put(c.id).put(net::make_name(c.name)).put(static_cast<std::uint8_t>(c.frames.size()));
    for (const Roll& r : c.frames) w.put(r.r[0]).put(r.r[1]).put(r.r[2]).put(r.n).put(r.last);
  }
  net.send_game(std::move(w.data()), true);
}

void host_tick(const Frame& f, net::Session& net, DWORD now) {
  const bool running = script::find_thread("tenpinbowl") != nullptr;
  if (!running) {
    if (g_view.active) {
      log::info("Bowling: script ended");
      reset_game();
      g_dirty = true;
    }
    g_locked_solo = false;
    g_phase = Phase::Aim;
    g_pins.clear();
    g_ball = 0;
  } else if (!g_view.active && !g_locked_solo && g_phase == Phase::Rolling && now - g_last_eval >= 200) {
    // The first throw decides who is in: partners can walk up while the host picks a lane.
    g_last_eval = now;
    const auto mates = partners_in_range(f);
    if (mates.empty()) {
      g_locked_solo = true;
      g_phase = Phase::Aim;
      g_pins.clear();
    } else {
      start_game(f, mates);
      begin_script_mode();
      // This first ball was the host's: score it once the pins settle (the phase carries on).
    }
  }
  if (g_view.active && !g_view.over) {
    // A player who left the session drops out of the rotation (their card stays).
    if (!g_script_mode && g_phase == Phase::Aim && !in_session(f, g_view.cur_id)) {
      advance_player(f);
      g_dirty = true;
    }
    if (g_phase == Phase::Rolling) track_roll(f, now);
    if (g_script_mode) script_tick(f, now);
  }
  if (g_view.over && now - g_over_at > kOverShowMs) {
    reset_game();
    g_locked_solo = true; // the script is wrapping up; don't start another game from its tail
    g_dirty = true;
  }
  if (g_dirty || (g_view.active && now - g_last_send >= kResendMs)) {
    g_dirty = false, g_last_send = now;
    send_state(net);
  }
}

// ---- hooks (host)

void on_create_object(const Call& c) {
  if (!g_hosting || !from_bowling(c)) return;
  const auto model = c.arg<std::int32_t>(0);
  const auto h = c.out<n::Object>(4);
  if (!h) return;
  if (model == kBallModel) {
    g_ball = h;
  } else if (model == kPinModel) {
    if (g_phase == Phase::Rolling) return; // the rack for the next throw isn't being set yet
    const DWORD now = GetTickCount();
    if (now - g_last_pin_at > kBurstMs) g_pins.clear();
    g_last_pin_at = now;
    g_pins.push_back({h, {}, {}, false});
  }
}

void on_release(const Call& c) {
  if (!g_hosting || !from_bowling(c)) return;
  if (!g_ball || c.arg<n::Object>(0) != g_ball || g_phase != Phase::Aim) return;
  // Snapshot the rack: standing pins only (the script may leave fallen ones lying until it sweeps).
  std::vector<Pin> rack;
  for (Pin& p : g_pins) {
    if (!p.h || !n::DoesObjectExist(p.h) || !n::invoke<bool>(kIsObjectUpright, p.h, kPinTilt)) continue;
    p.start = p.last = n::GetObjectCoordinates(p.h);
    p.down = false;
    rack.push_back(p);
  }
  g_pins = std::move(rack);
  g_standing_before = static_cast<int>(g_pins.size());
  g_phase = Phase::Rolling;
  g_release_at = g_last_move_at = GetTickCount();
  if (g_standing_before == 0) log::warn("Bowling: ball released but no pins were seen (rack missed)");
}

// While it is a partner's turn the script's input reads are answered with the partner's.
bool partner_controls() {
  return g_hosting && g_view.active && !g_view.over && g_view.cur_id != g_my_id && g_view.cur_id != kFriendId &&
         !g_proxy;
}
bool input_fresh() { return GetTickCount() - g_in_at < kInputStaleMs; }

void on_sticks(const Call& c) {
  if (!partner_controls() || !from_bowling(c)) return;
  const bool fresh = input_fresh();
  const std::int32_t v[4] = {fresh ? g_in.lx : 0, fresh ? g_in.ly : 0, fresh ? g_in.rx : 0, fresh ? g_in.ry : 0};
  for (int i = 0; i < 4; ++i)
    if (auto* p = c.arg<std::int32_t*>(1 + i)) *p = v[i];
}

void on_mouse(const Call& c) {
  if (!partner_controls() || !from_bowling(c)) return;
  const bool fresh = input_fresh();
  if (auto* p = c.arg<std::int32_t*>(0)) *p = fresh ? g_in.mx : 0;
  if (auto* p = c.arg<std::int32_t*>(1)) *p = fresh ? g_in.my : 0;
}

void on_control(const Call& c, bool just) {
  if (!partner_controls() || !from_bowling(c)) return;
  const int i = ctl_index(c.arg<std::int32_t>(0), c.arg<std::int32_t>(1));
  if (i < 0) return;
  bool v;
  if (just) {
    v = (g_in_just & (1u << i)) != 0;
    g_in_just &= static_cast<std::uint8_t>(~(1u << i));
  } else {
    v = input_fresh() && (g_in.pressed & (1u << i));
  }
  *reinterpret_cast<std::int32_t*>(c.ctx->result) = v ? 1 : 0;
}

void on_using_controller(const Call& c) {
  if (!partner_controls() || !from_bowling(c)) return;
  *reinterpret_cast<std::int32_t*>(c.ctx->result) = (input_fresh() && g_in.controller) ? 1 : 0;
}

// ---- partner: sample and send our input while it is our turn

void send_input(net::Session& net, DWORD now) {
  std::int32_t lx = 0, ly = 0, rx = 0, ry = 0, mx = 0, my = 0;
  n::invoke(kGetSticks, 0, &lx, &ly, &rx, &ry);
  n::invoke(kGetMouse, &mx, &my);
  std::uint8_t pressed = 0, just = 0;
  for (int i = 0; i < kNumCtl; ++i) {
    if (n::invoke<bool>(kIsControlPressed, kCtl[i].pad, kCtl[i].control)) pressed |= std::uint8_t(1u << i);
    if (n::invoke<bool>(kIsControlJustPressed, kCtl[i].pad, kCtl[i].control)) just |= std::uint8_t(1u << i);
  }
  g_pending_just |= just;
  if (now - g_sent_at < kInputMs && !just) return;
  g_sent_at = now;
  const bool controller = n::invoke<bool>(kIsUsingController);
  auto s16 = [](std::int32_t v) { return static_cast<std::int16_t>(std::clamp(v, -32768, 32767)); };
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(std::uint8_t{OpInput}).put(pressed).put(g_pending_just)
      .put(static_cast<std::uint8_t>(controller ? 1 : 0)).put(s16(lx)).put(s16(ly)).put(s16(rx)).put(s16(ry))
      .put(s16(mx)).put(s16(my));
  net.send_game(std::move(w.data()), g_pending_just != 0); // a button edge must not get lost
  g_pending_just = 0;
}

// ---- scorecard overlay (every machine, every frame)

void text(float x, float y, const std::string& s, int r, int g, int b, float scale) {
  n::invoke(kSetTextFont, 0);
  n::invoke(kSetTextScale, scale * 0.75f, scale);
  n::invoke(kSetTextProportional, 1);
  n::invoke(kSetTextBackground, 0);
  n::invoke(kSetTextDropshadow, 1, 0, 0, 0, 255);
  n::invoke(kSetTextWrap, 0.0f, 1.0f);
  n::invoke(kSetTextColour, r, g, b, 255);
  n::invoke(kDisplayLiteral, x, y, "STRING", s.c_str());
}

void draw(std::uint8_t my_id) {
  const View& v = g_view;
  if (!v.active) return;
  constexpr float x0 = 0.03f, y0 = 0.30f, row = 0.034f;
  const float h = row * static_cast<float>(v.cards.size() + 3);
  n::invoke(kDrawRect, 0.24f, y0 + h * 0.5f - 0.01f, 0.44f, h + 0.01f, 0, 0, 0, 150);
  text(x0, y0, v.over ? std::string("TENPIN BOWLING - FINAL") : std::format("TENPIN BOWLING  frame {}/{}", v.frame + 1, kFrames),
       255, 255, 255, 0.30f);
  float y = y0 + row;
  for (const Card& c : v.cards) {
    const bool cur = c.id == v.cur_id && !v.over;
    std::string line = std::format("{} {:<10.10}", cur ? ">" : " ", c.name);
    for (const Roll& f : c.frames) line += std::format("  {:<3}", frame_marks(f));
    line += std::format("   = {}", score(c.frames));
    text(x0, y, line, cur ? 255 : 230, cur ? 215 : 230, cur ? 70 : 230, 0.27f);
    y += row;
  }
  if (v.over) return;
  if (v.cur_id == my_id && my_id != net::kHostId) {
    // The script reads the aim sideways and the throw as a pull-back then push-forward gesture (hold the fire /
    // action control while doing it); a partner's input reaches it from here.
    const bool pad = n::invoke<bool>(kIsUsingController);
    text(x0, y + 0.004f, "YOUR TURN", 120, 255, 120, 0.30f);
    text(x0, y + 0.004f + row,
         pad ? "Left stick left/right: aim." : "Mouse left/right: aim.", 120, 255, 120, 0.26f);
    text(x0, y + 0.004f + row * 2,
         pad ? "Hold the throw button, pull the stick back, then push it forward."
             : "Hold fire, pull the mouse back, then push it forward to throw.",
         120, 255, 120, 0.26f);
  } else if (v.cur_id != my_id) {
    for (const Card& c : v.cards)
      if (c.id == v.cur_id) text(x0, y + 0.004f, std::format("{} is bowling", c.name), 200, 200, 200, 0.26f);
  }
}

} // namespace

bool lane_cam_wanted() {
  if (!g_hosting || !g_view.active) return false;
  return std::any_of(g_view.cards.begin(), g_view.cards.end(),
                     [](const Card& c) { return c.id != g_my_id && c.id != kFriendId; });
}

bool participating() {
  if (g_hosting || !g_view.active || !g_rx_valid) return false;
  return std::any_of(g_view.cards.begin(), g_view.cards.end(), [](const Card& c) { return c.id == g_my_id; });
}

int score(const std::vector<Roll>& frames) {
  int total = 0;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const Roll& f = frames[i];
    total += pins_of(f);
    if (f.last) continue;
    const auto later = later_rolls(frames, i);
    if (f.n == 1 && f.r[0] == 10) {
      for (std::size_t k = 0; k < later.size() && k < 2; ++k) total += later[k];
    } else if (f.n == 2 && f.r[0] + f.r[1] == 10) {
      if (!later.empty()) total += later[0];
    }
  }
  return total;
}

void register_hooks() {
  native_hooks::on(kCreateObjectNoOffset, "CREATE_OBJECT_NO_OFFSET", on_create_object);
  native_hooks::on(kSetObjectInitialVelocity, "SET_OBJECT_INITIAL_VELOCITY", on_release);
  native_hooks::on(kGetSticks, "GET_POSITION_OF_ANALOGUE_STICKS", on_sticks);
  native_hooks::on(kGetMouse, "GET_MOUSE_INPUT", on_mouse);
  native_hooks::on(kIsControlPressed, "IS_CONTROL_PRESSED", [](const Call& c) { on_control(c, false); });
  native_hooks::on(kIsControlJustPressed, "IS_CONTROL_JUST_PRESSED", [](const Call& c) { on_control(c, true); });
  native_hooks::on(kIsUsingController, "IS_USING_CONTROLLER", on_using_controller);
}

void tick(const Frame& f, net::Session& net) {
  const DWORD now = GetTickCount();
  g_hosting = f.in_session && f.hosting;
  g_my_id = f.my_id;
  if (!f.in_session) {
    if (g_froze) {
      n::invoke(kSetPlayerControl, n::GetPlayerId(), true);
      g_froze = false;
    }
    if (g_view.active || g_rx_valid) {
      reset_game();
      g_rx_valid = false;
    }
    g_locked_solo = false;
    return;
  }
  if (f.hosting) {
    host_tick(f, net, now);
  } else if (g_view.active && g_rx_valid && now - g_rx_at > kStaleMs) {
    g_view = View{}; // the host stopped sending: its game is over
    g_rx_valid = false;
  }
  const bool my_turn = !f.hosting && g_view.active && !g_view.over && g_view.cur_id == f.my_id;
  if (my_turn) send_input(net, now);
  // Our character stands still while we aim (the throw is the host's character's; the host's script has the lane).
  if (my_turn != g_froze) {
    n::invoke(kSetPlayerControl, n::GetPlayerId(), !my_turn);
    g_froze = my_turn;
  }
  draw(f.my_id);
}

void on_message(const net::GameMessage& m) {
  net::Reader r(m.data);
  std::uint8_t type, sender, op;
  if (!r.get(type) || !r.get(sender) || !r.get(op)) return;
  if (op == OpInput) {
    // Host: the controlling partner's input.
    if (!g_hosting || !g_view.active || m.from != g_view.cur_id) return;
    Input in;
    std::uint8_t controller;
    std::int16_t lx, ly, rx, ry, mx, my;
    if (!r.get(in.pressed) || !r.get(in.just) || !r.get(controller) || !r.get(lx) || !r.get(ly) || !r.get(rx) ||
        !r.get(ry) || !r.get(mx) || !r.get(my))
      return;
    in.controller = controller != 0;
    in.lx = lx, in.ly = ly, in.rx = rx, in.ry = ry, in.mx = mx, in.my = my;
    g_in_just |= in.just;
    in.just = 0;
    g_in = in;
    g_in_at = GetTickCount();
    return;
  }
  if (op != OpState || m.from != net::kHostId || g_hosting) return;
  std::uint8_t seq, active, over, cur, frame, count;
  if (!r.get(seq) || !r.get(active) || !r.get(over) || !r.get(cur) || !r.get(frame) || !r.get(count) || count > 8)
    return;
  if (g_rx_valid && static_cast<std::int8_t>(seq - g_rx_seq) <= 0) return; // old or repeated
  View v;
  v.active = active != 0, v.over = over != 0, v.cur_id = cur, v.frame = frame;
  for (int i = 0; i < count; ++i) {
    Card c;
    net::Name name{};
    std::uint8_t nf;
    if (!r.get(c.id) || !r.get(name) || !r.get(nf) || nf > kFrames) return;
    c.name = net::to_string(name);
    for (int k = 0; k < nf; ++k) {
      Roll f;
      if (!r.get(f.r[0]) || !r.get(f.r[1]) || !r.get(f.r[2]) || !r.get(f.n) || !r.get(f.last) || f.n > 3) return;
      c.frames.push_back(f);
    }
    v.cards.push_back(std::move(c));
  }
  g_rx_seq = seq, g_rx_valid = true, g_rx_at = GetTickCount();
  g_view = std::move(v);
}

} // namespace ivc::bowling_sync
