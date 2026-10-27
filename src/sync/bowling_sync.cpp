#include "sync/bowling_sync.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>

#include "core/log.h"
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
                        kDisplayLiteral = 0x661B239A, kDrawRect = 0x3B2526E3;
// tenpinbowl's models (CREATE_OBJECT_NO_OFFSET): the pin and the ball.
constexpr std::int32_t kPinModel = -190707996, kBallModel = -1862626618;
// The script's own "pin is down" test (sub_65157): moved over 0.2 m on the ground, or tilted over 15 degrees.
constexpr float kPinMoved = 0.2f, kPinTilt = 15.0f;
constexpr DWORD kBurstMs = 700; // pins created closer together than this are one rack
constexpr DWORD kMinRollMs = 2500, kQuietMs = 1200, kMaxRollMs = 14000;
constexpr DWORD kInputMs = 33, kInputStaleMs = 500, kResendMs = 2000, kStaleMs = 6000, kOverShowMs = 10000;
constexpr int kFrames = 10;

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

// Partner.
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

void reset_game() {
  g_view = View{};
  g_ball_in_frame = 0;
  g_pins.clear();
  g_phase = Phase::Aim;
  g_in = {}, g_in_just = 0;
}

Card* card_of(std::uint8_t id) {
  for (Card& c : g_view.cards)
    if (c.id == id) return &c;
  return nullptr;
}

bool in_session(const Frame& f, std::uint8_t id) {
  return id == g_my_id || std::any_of(f.players.begin(), f.players.end(), [&](const Player& p) { return p.id == id; });
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

void finish_roll(const Frame& f) {
  int down = 0;
  for (const Pin& p : g_pins)
    if (p.down) ++down;
  g_phase = Phase::Aim;
  g_pins.clear(); // a rack is only valid for the throw it was taken for
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
      // This first ball was the host's: score it once the pins settle (the phase carries on).
    }
  }
  if (g_view.active && !g_view.over) {
    // A player who left the session drops out of the rotation (their card stays).
    if (g_phase == Phase::Aim && !in_session(f, g_view.cur_id)) {
      advance_player(f);
      g_dirty = true;
    }
    if (g_phase == Phase::Rolling) track_roll(f, now);
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
bool partner_controls() { return g_hosting && g_view.active && !g_view.over && g_view.cur_id != g_my_id; }
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
    text(x0, y + 0.004f, "Your turn: pull back, then push forward on the stick", 120, 255, 120, 0.26f);
  } else if (v.cur_id != my_id) {
    for (const Card& c : v.cards)
      if (c.id == v.cur_id) text(x0, y + 0.004f, std::format("{} is bowling", c.name), 200, 200, 200, 0.26f);
  }
}

} // namespace

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
  if (!f.hosting && g_view.active && !g_view.over && g_view.cur_id == f.my_id) send_input(net, now);
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
