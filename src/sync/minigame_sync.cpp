#include "sync/minigame_sync.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

#include "core/log.h"
#include "game/native_hooks.h"
#include "game/phone.h"
#include "game/script.h"
#include "net/bytes.h"

namespace ivc::minigame_sync {
namespace {
namespace n = natives;
using native_hooks::Call;

constexpr std::uint32_t kGetSticks = 0x4F7F4FAE, kGetMouse = 0x447B154B, kIsControlPressed = 0x0E635761,
                        kIsControlJustPressed = 0x4CB729F1, kIsUsingController = 0x669D053F,
                        kIsButtonPressed = 0x7FF21081, kIsMouseButtonPressed = 0x39E600D0,
                        kSetPlayerControl = 0x1A6203EA, kGetKeyboardMove = 0x4AF73456;
constexpr DWORD kTurnGapMs = 1500;   // no aim reads for this long: the go is over
constexpr DWORD kInputMs = 33, kInputStaleMs = 400, kResendMs = 2000;
constexpr int kMaxButton = 31;

enum Op : std::uint8_t { OpState = 0, OpInput = 1 };

const char* const kScripts[] = {"pool_game", "darts"};
const char* const kGameNames[] = {"pool", "darts"};

// The script's control reads a partner answers for: (pad, control). Index = bit in the masks.
struct Ctl {
  int pad, control;
};
constexpr Ctl kCtl[] = {{2, 0}, {2, 1}, {2, 2}, {2, 4}, {2, 23}, {2, 77}, {0, 77}, {2, 78}, {0, 78}};
constexpr int kNumCtl = static_cast<int>(sizeof(kCtl) / sizeof(kCtl[0]));

int ctl_index(int pad, int control) {
  for (int i = 0; i < kNumCtl; ++i)
    if (kCtl[i].pad == pad && kCtl[i].control == control) return i;
  return -1;
}

struct Input {
  std::uint16_t controls = 0;
  std::uint32_t buttons = 0;
  std::uint8_t mouse_buttons = 0;
  bool controller = false;
  std::int32_t lx = 0, ly = 0, rx = 0, ry = 0, mx = 0, my = 0, kx = 0, ky = 0;
};

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

// Shared state (host: authoritative; partners: the last received).
bool g_active = false;
std::uint8_t g_game = 0;       // index into kScripts
std::uint8_t g_controller = 0; // whose go it is
std::uint8_t g_players = 0;    // bit per session id at the table

// Host.
bool g_hosting = false;
std::uint8_t g_my_id = 0;
bool g_locked_solo = false;      // the script started with nobody else near: vanilla until it ends
std::vector<std::uint8_t> g_order;
std::size_t g_turn = 0;
DWORD g_last_aim = 0;
bool g_in_go = false;            // the current controller has had aim reads since their go began
bool g_dirty = false;
DWORD g_last_send = 0;
Input g_in;
std::uint16_t g_in_just = 0; // latched just-pressed bits, cleared as the script reads them
DWORD g_in_at = 0;

// Partner.
bool g_froze = false;
DWORD g_sent_at = 0;
std::uint16_t g_pending_just = 0;
std::uint8_t g_seen_controller = 0xFF;

bool from_minigame(const Call& c) {
  if (!g_active) return false;
  return lower(c.script) == kScripts[g_game];
}

bool partner_controls() { return g_hosting && g_active && g_controller != g_my_id; }
bool input_fresh() { return GetTickCount() - g_in_at < kInputStaleMs; }

// Host: the script read the aim input; a read after a pause starts the next player's go.
void aim_read() {
  const DWORD now = GetTickCount();
  if (g_in_go && now - g_last_aim > kTurnGapMs && g_order.size() > 1) {
    g_turn = (g_turn + 1) % g_order.size();
    g_controller = g_order[g_turn];
    g_in = Input{}, g_in_just = 0;
    g_dirty = true;
    log::info("Minigame: {}'s go goes to player {}", kGameNames[g_game], g_controller);
  }
  g_in_go = true;
  g_last_aim = now;
}

void on_sticks(const Call& c) {
  if (!g_hosting || !from_minigame(c)) return;
  aim_read();
  if (!partner_controls()) return;
  const bool fresh = input_fresh();
  const std::int32_t v[4] = {fresh ? g_in.lx : 0, fresh ? g_in.ly : 0, fresh ? g_in.rx : 0, fresh ? g_in.ry : 0};
  for (int i = 0; i < 4; ++i)
    if (auto* p = c.arg<std::int32_t*>(1 + i)) *p = v[i];
}

void on_mouse(const Call& c) {
  if (!g_hosting || !from_minigame(c)) return;
  aim_read();
  if (!partner_controls()) return;
  const bool fresh = input_fresh();
  if (auto* p = c.arg<std::int32_t*>(0)) *p = fresh ? g_in.mx : 0;
  if (auto* p = c.arg<std::int32_t*>(1)) *p = fresh ? g_in.my : 0;
}

// Pool aims with the keyboard's move keys when the sticks are idle.
void on_keyboard_move(const Call& c) {
  if (!g_hosting || !from_minigame(c)) return;
  aim_read();
  if (!partner_controls()) return;
  const bool fresh = input_fresh();
  if (auto* p = c.arg<std::int32_t*>(0)) *p = fresh ? g_in.kx : 0;
  if (auto* p = c.arg<std::int32_t*>(1)) *p = fresh ? g_in.ky : 0;
}

void on_control(const Call& c, bool just) {
  if (!partner_controls() || !from_minigame(c)) return;
  const int i = ctl_index(c.arg<std::int32_t>(0), c.arg<std::int32_t>(1));
  if (i < 0) return;
  bool v;
  if (just) {
    v = (g_in_just & (1u << i)) != 0;
    g_in_just &= static_cast<std::uint16_t>(~(1u << i));
  } else {
    v = input_fresh() && (g_in.controls & (1u << i));
  }
  *reinterpret_cast<std::int32_t*>(c.ctx->result) = v ? 1 : 0;
}

void on_button(const Call& c) {
  if (!partner_controls() || !from_minigame(c) || c.arg<std::int32_t>(0) != 0) return;
  const int b = c.arg<std::int32_t>(1);
  if (b < 0 || b > kMaxButton) return;
  *reinterpret_cast<std::int32_t*>(c.ctx->result) = input_fresh() && (g_in.buttons & (1u << b)) ? 1 : 0;
}

void on_mouse_button(const Call& c) {
  if (!partner_controls() || !from_minigame(c)) return;
  const int b = c.arg<std::int32_t>(0);
  if (b < 0 || b > 7) return;
  *reinterpret_cast<std::int32_t*>(c.ctx->result) = input_fresh() && (g_in.mouse_buttons & (1u << b)) ? 1 : 0;
}

void on_using_controller(const Call& c) {
  if (!partner_controls() || !from_minigame(c)) return;
  *reinterpret_cast<std::int32_t*>(c.ctx->result) = (input_fresh() && g_in.controller) ? 1 : 0;
}

n::Ped player_ped() {
  const n::Ped p = n::GetPlayerChar(n::GetPlayerId());
  return p && n::DoesCharExist(p) ? p : 0;
}

void send_state(net::Session& net) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(std::uint8_t{OpState}).put(static_cast<std::uint8_t>(g_active))
      .put(g_game).put(g_controller).put(g_players);
  net.send_game(std::move(w.data()), true);
}

std::string name_of(const Frame& f, std::uint8_t id) {
  for (const auto& p : f.players)
    if (p.id == id) return p.name;
  return "?";
}

void host_tick(const Frame& f, net::Session& net, DWORD now) {
  // Which minigame script is running, if any.
  int running = -1;
  for (int i = 0; i < 2; ++i)
    if (script::find_thread(kScripts[i])) running = i;
  if (running < 0) {
    if (g_active) {
      log::info("Minigame: {} ended", kGameNames[g_game]);
      g_active = false, g_dirty = true;
    }
    g_locked_solo = false;
  } else if (!g_active && !g_locked_solo) {
    // Starting: who is at the table (the host, then partners near the host's player).
    g_order.assign(1, f.my_id);
    const n::Ped me = player_ped();
    n::Vector3 at{};
    if (me) at = n::GetCharCoordinates(me);
    for (const auto& p : f.players) {
      if (p.id == f.my_id || !p.ped || !n::DoesCharExist(p.ped)) continue;
      const n::Vector3 q = n::GetCharCoordinates(p.ped);
      if (std::hypot(q.x - at.x, q.y - at.y) <= kPresentRange) g_order.push_back(p.id);
    }
    if (g_order.size() < 2) {
      g_locked_solo = true; // nobody to play with: the game stays single player until it ends
    } else {
      g_active = true, g_game = static_cast<std::uint8_t>(running), g_turn = 0, g_controller = f.my_id;
      g_in_go = false, g_dirty = true;
      log::info("Minigame: {} with {} players", kGameNames[g_game], g_order.size());
      phone::notify(std::format("Co-op {}: everyone here plays your side, taking turns.", kGameNames[g_game]));
    }
  }
  if (g_active) {
    // A player who left: their go passes on.
    std::erase_if(g_order, [&](std::uint8_t id) {
      return id != f.my_id && std::none_of(f.players.begin(), f.players.end(), [id](const Player& p) { return p.id == id; });
    });
    if (std::find(g_order.begin(), g_order.end(), g_controller) == g_order.end()) {
      g_turn = 0, g_controller = f.my_id, g_dirty = true;
    }
  }
  if (g_dirty) {
    if (g_active && g_controller != g_seen_controller)
      phone::notify(g_controller == f.my_id ? std::format("Your go ({}).", kGameNames[g_game])
                                            : std::format("{}'s go ({}).", name_of(f, g_controller), kGameNames[g_game]));
    g_seen_controller = g_active ? g_controller : 0xFF;
  }
  g_players = 0;
  if (g_active)
    for (std::uint8_t id : g_order)
      if (id < 8) g_players |= std::uint8_t(1u << id);
  if (g_dirty || (g_active && now - g_last_send > kResendMs)) {
    send_state(net);
    g_dirty = false, g_last_send = now;
  }
}

// Partner: sample and send our input while it's our go.
void send_input(net::Session& net, DWORD now) {
  std::int32_t lx = 0, ly = 0, rx = 0, ry = 0, mx = 0, my = 0;
  n::invoke(kGetSticks, 0, &lx, &ly, &rx, &ry);
  n::invoke(kGetMouse, &mx, &my);
  std::int32_t kx = 0, ky = 0;
  n::invoke(kGetKeyboardMove, &kx, &ky);
  std::uint16_t controls = 0, just = 0;
  for (int i = 0; i < kNumCtl; ++i) {
    if (n::invoke<bool>(kIsControlPressed, kCtl[i].pad, kCtl[i].control)) controls |= std::uint16_t(1u << i);
    if (n::invoke<bool>(kIsControlJustPressed, kCtl[i].pad, kCtl[i].control)) just |= std::uint16_t(1u << i);
  }
  std::uint32_t buttons = 0;
  for (int b = 0; b <= 19; ++b)
    if (n::invoke<bool>(kIsButtonPressed, 0, b)) buttons |= 1u << b;
  std::uint8_t mouse_buttons = 0;
  for (int b = 1; b <= 2; ++b)
    if (n::invoke<bool>(kIsMouseButtonPressed, b)) mouse_buttons |= std::uint8_t(1u << b);
  g_pending_just |= just;
  if (now - g_sent_at < kInputMs && !just) return;
  g_sent_at = now;
  const bool controller = n::invoke<bool>(kIsUsingController);
  auto s16 = [](std::int32_t v) { return static_cast<std::int16_t>(std::clamp(v, -32768, 32767)); };
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(std::uint8_t{OpInput}).put(controls).put(g_pending_just).put(buttons)
      .put(mouse_buttons).put(static_cast<std::uint8_t>(controller ? 1 : 0)).put(s16(lx)).put(s16(ly)).put(s16(rx))
      .put(s16(ry)).put(s16(mx)).put(s16(my)).put(s16(kx)).put(s16(ky));
  net.send_game(std::move(w.data()), g_pending_just != 0); // a button edge must not get lost
  g_pending_just = 0;
}

} // namespace

bool cam_wanted() { return g_hosting && g_active; }

bool participating() { return !g_hosting && g_active && g_my_id < 8 && (g_players & (1u << g_my_id)); }

void register_hooks() {
  native_hooks::on(kGetSticks, "GET_POSITION_OF_ANALOGUE_STICKS", on_sticks);
  native_hooks::on(kGetMouse, "GET_MOUSE_INPUT", on_mouse);
  native_hooks::on(kGetKeyboardMove, "GET_KEYBOARD_MOVE_INPUT", on_keyboard_move);
  native_hooks::on(kIsControlPressed, "IS_CONTROL_PRESSED", [](const Call& c) { on_control(c, false); });
  native_hooks::on(kIsControlJustPressed, "IS_CONTROL_JUST_PRESSED", [](const Call& c) { on_control(c, true); });
  native_hooks::on(kIsButtonPressed, "IS_BUTTON_PRESSED", on_button);
  native_hooks::on(kIsMouseButtonPressed, "IS_MOUSE_BUTTON_PRESSED", on_mouse_button);
  native_hooks::on(kIsUsingController, "IS_USING_CONTROLLER", on_using_controller);
}

void tick(const Frame& f, net::Session& net) {
  const DWORD now = GetTickCount();
  g_hosting = f.in_session && f.hosting;
  g_my_id = f.my_id;
  if (!f.in_session) {
    if (g_froze) n::invoke(kSetPlayerControl, n::GetPlayerId(), true), g_froze = false;
    g_active = false, g_locked_solo = false, g_seen_controller = 0xFF;
    return;
  }
  if (f.hosting) host_tick(f, net, now);
  const bool my_go = !f.hosting && g_active && g_controller == f.my_id;
  if (!f.hosting && g_active && g_controller != g_seen_controller) {
    g_seen_controller = g_controller;
    phone::notify(my_go ? std::format("Your go at {}: aim and shoot as usual.", kGameNames[g_game])
                        : std::format("{}: {}'s go.", kGameNames[g_game],
                                      g_controller == net::kHostId ? std::string("the host") : name_of(f, g_controller)));
  }
  if (my_go) send_input(net, now);
  // Our character stands still on our go (the shot is the host character's; the host's script has the table).
  if (my_go != g_froze) {
    n::invoke(kSetPlayerControl, n::GetPlayerId(), !my_go);
    g_froze = my_go;
  }
}

void on_message(const net::GameMessage& m) {
  net::Reader r(m.data);
  std::uint8_t type, sender, op;
  if (!r.get(type) || !r.get(sender) || !r.get(op)) return;
  if (op == OpInput) {
    if (!g_hosting || !g_active || m.from != g_controller) return;
    Input in;
    std::uint16_t just;
    std::uint8_t controller;
    std::int16_t lx, ly, rx, ry, mx, my, kx, ky;
    if (!r.get(in.controls) || !r.get(just) || !r.get(in.buttons) || !r.get(in.mouse_buttons) || !r.get(controller) ||
        !r.get(lx) || !r.get(ly) || !r.get(rx) || !r.get(ry) || !r.get(mx) || !r.get(my) || !r.get(kx) || !r.get(ky))
      return;
    in.controller = controller != 0;
    in.lx = lx, in.ly = ly, in.rx = rx, in.ry = ry, in.mx = mx, in.my = my, in.kx = kx, in.ky = ky;
    g_in_just |= just;
    g_in = in;
    g_in_at = GetTickCount();
    return;
  }
  if (op != OpState || m.from != net::kHostId || g_hosting) return;
  std::uint8_t active, game, controller, players;
  if (!r.get(active) || !r.get(game) || !r.get(controller) || !r.get(players) || game > 1) return;
  g_active = active != 0, g_game = game, g_controller = controller, g_players = players;
  if (!g_active) g_seen_controller = 0xFF;
}

} // namespace ivc::minigame_sync
