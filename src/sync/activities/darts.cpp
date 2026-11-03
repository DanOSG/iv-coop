#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <format>
#include <vector>

#include "core/log.h"
#include "game/episode.h"
#include "game/natives.h"
#include "game/phone.h"
#include "net/bytes.h"
#include "net/protocol.h"
#include "net/session.h"
#include "sync/activities/activities.h"
#include "sync/activities/darts.h"
#include "sync/activity_seats.h"

// Darts for up to four humans, each with their own 301 (double out). The script has two seats hard-coded (seat 0 the
// local player, seat 1 the AI), so every human plays through seat 0, the "human window": at each seat flip (3 darts or a
// bust) the window's {score, history row, turn-start score} is saved for the player who just threw, the next human's
// are loaded, and the script is put back on seat 0 so the AI never throws. A partner's turn is activity_seats'
// set_active(partner): their input answers the script's reads and GET_PLAYER_CHAR is their puppet, so their character
// stands at the oche. The script's own board only has two rows, so the mod draws its own and sends it to the partners
// (message 77). Slot numbers and evidence: docs/activities/darts.md, docs/ARCHITECTURE.md ("Darts").
namespace ivc::activities::darts {

namespace {
namespace n = natives;

constexpr std::uint32_t kSetTextScale = 0x02C069E5, kSetTextColour = 0x19C967B5, kSetTextDropshadow = 0x58F5023F,
                        kSetTextFont = 0x75363BB5, kSetTextWrap = 0x19D006EB, kSetTextProportional = 0x15585A65,
                        kSetTextBackground = 0x768F5140, kDisplayLiteral = 0x661B239A, kDrawRect = 0x3B2526E3;
constexpr int kStart = 301, kHistCols = 7, kMaxHumans = 4;
constexpr std::uint8_t kNobody = 0xFF;
constexpr DWORD kResendMs = 2000, kStaleMs = 6000;

// The script's statics (IV numbering from the decompiled darts.txt; l_U<n> = static n, an array's count word is its
// static and element k is n+1+k). IV: state l_U0 (:2518 `l_U0 = 2`), sub-state l_U1, current seat l_U331, dart number
// l_U332, score[2] l_U328 (init 301 :1343-1344), turn-start score[2] l_U252 (bust revert :2355, written :2986), history
// [2][7] l_U336 (init :1334-1342; row s col c = n+2+8s+c), points this turn l_U354, checkout hint l_U369 (both reset in
// sub_32182 :2987-2988 and :1361-1362). TLAD (state l_U0 :2518, seat flip :2945-2967, init :1386-1396) has the same
// numbers; TBoGT is IV + 2 throughout (state l_U2 :2557, seat l_U333, dart l_U334, score l_U330 :1434-1435, turn-start
// l_U254, history l_U338 :1425-1433, points l_U356, hint l_U371, flip :2974-2996).
struct Layout {
  int state, sub, seat, dart, score, turn_start, hist, points, hint;
};
constexpr Layout kLayoutIV{0, 1, 331, 332, 328, 252, 336, 354, 369};
constexpr Layout kLayoutTLAD{0, 1, 331, 332, 328, 252, 336, 354, 369};
constexpr Layout kLayoutTBoGT{2, 3, 333, 334, 330, 254, 338, 356, 371};

struct Human {
  std::uint8_t id = 0;
  std::string name;
  n::Ped ped = 0;                  // the partner's puppet; 0 for the host
  std::int32_t score = kStart;     // saved while another human is in the window
  std::int32_t hist[kHistCols] = {kStart, 0, 0, 0, 0, 0, 0};
  std::int32_t turn_start = kStart;
  bool gone = false;               // left the session or the table: skipped
};

// The board as everyone sees it.
struct View {
  bool active = false, over = false;
  std::uint8_t cur_id = 0, winner_id = 0;
  struct Row {
    std::uint8_t id;
    std::string name;
    std::int16_t score;
  };
  std::vector<Row> rows;
};

// Host engine.
enum class Mode { Idle, Vanilla, Playing } g_mode = Mode::Idle;
std::int32_t* g_st = nullptr; // the statics the game was set up in
Layout g_lay{};
std::vector<Human> g_humans;
std::size_t g_cur = 0;
int g_prev_seat = -1, g_prev_state = -1;
bool g_fix_dart = false; // after a rotation: undo the script's bust-path `l_U332++` until the first throw
bool g_over = false;
std::uint8_t g_winner = 0;
std::uint8_t g_my_id = 0;
View g_view;
bool g_dirty = false;
std::uint8_t g_seq = 0;
DWORD g_last_send = 0;

// Everyone else.
View g_rx;
bool g_rx_valid = false;
std::uint8_t g_rx_seq = 0;
DWORD g_rx_at = 0;
DWORD g_host_tick_at = 0; // the last frame host_tick ran: we're the host (client_tick draws only when it hasn't)

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

bool pick_layout() {
  switch (episode::current()) {
    case episode::Id::IV: g_lay = kLayoutIV; return true;
    case episode::Id::TLAD: g_lay = kLayoutTLAD; return true;
    case episode::Id::TBoGT: g_lay = kLayoutTBoGT; return true;
    default: return false;
  }
}

std::int32_t& st(int idx) { return g_st[idx]; }
std::int32_t& hist(int row, int col) { return g_st[g_lay.hist + 2 + 8 * row + col]; }
std::int32_t& score(int seat) { return g_st[g_lay.score + 1 + seat]; }
std::int32_t& turn_start(int seat) { return g_st[g_lay.turn_start + 1 + seat]; }

// Array count words (the script's own array sizes) and plausible values, or why not.
bool layout_ok(std::string& why) {
  if (st(g_lay.score) != 2 || st(g_lay.turn_start) != 2) return why = "score array size words", false;
  if (st(g_lay.hist) != 2 || st(g_lay.hist + 1) != kHistCols || st(g_lay.hist + 9) != kHistCols)
    return why = "history array size words", false;
  if (st(g_lay.state) < 0 || st(g_lay.state) > 2) return why = "game state", false;
  if (st(g_lay.seat) < 0 || st(g_lay.seat) > 1) return why = "current seat", false;
  for (int s = 0; s < 2; ++s)
    if (score(s) < 0 || score(s) > kStart) return why = "score", false;
  return true;
}

std::string name_of(const Frame& f, std::uint8_t id) {
  for (const Player& p : f.players)
    if (p.id == id) return p.name.empty() ? std::format("Player {}", id) : p.name;
  return std::format("Player {}", id);
}

std::uint8_t id_for_active(const Human& h) { return h.id == g_my_id ? kNobody : h.id; }

void drop_game(const char* why) {
  for (const Human& h : g_humans)
    if (h.id != g_my_id) activity_seats::unseat(h.id);
  activity_seats::set_active(kNobody);
  if (g_mode == Mode::Playing) log::info("Darts: game ended ({})", why);
  g_humans.clear();
  g_cur = 0;
  g_over = false;
  g_fix_dart = false;
  g_prev_seat = g_prev_state = -1;
  if (g_view.active) g_dirty = true;
  g_view = View{};
  g_view.active = false;
}

void write_window(const Human& h) {
  score(0) = h.score;
  turn_start(0) = h.turn_start;
  for (int c = 0; c < kHistCols; ++c) hist(0, c) = h.hist[c];
}

void read_window(Human& h) {
  h.score = score(0);
  h.turn_start = turn_start(0);
  for (int c = 0; c < kHistCols; ++c) h.hist[c] = hist(0, c);
}

void fresh_players() {
  for (Human& h : g_humans) {
    h.score = h.turn_start = kStart;
    for (int c = 0; c < kHistCols; ++c) h.hist[c] = c == 0 ? kStart : 0;
  }
}

void rebuild_view() {
  View v;
  v.active = g_mode == Mode::Playing;
  v.over = g_over;
  if (!g_humans.empty()) v.cur_id = g_humans[g_cur].id;
  v.winner_id = g_winner;
  for (std::size_t i = 0; i < g_humans.size(); ++i) {
    const Human& h = g_humans[i];
    v.rows.push_back({h.id, h.name, static_cast<std::int16_t>(i == g_cur && st(g_lay.state) != 0 ? score(0) : h.score)});
  }
  const bool same = v.active == g_view.active && v.over == g_view.over && v.cur_id == g_view.cur_id &&
                    v.winner_id == g_view.winner_id && v.rows.size() == g_view.rows.size() &&
                    std::equal(v.rows.begin(), v.rows.end(), g_view.rows.begin(),
                               [](const View::Row& a, const View::Row& b) { return a.id == b.id && a.score == b.score; });
  if (!same) g_dirty = true;
  g_view = std::move(v);
}

// ---- drawing (every machine; the same board)

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

void draw(const View& v) {
  if (!v.active || v.rows.empty()) return;
  constexpr float x0 = 0.03f, y0 = 0.30f, row = 0.034f;
  const float h = row * static_cast<float>(v.rows.size() + 2);
  n::invoke(kDrawRect, 0.14f, y0 + h * 0.5f - 0.01f, 0.24f, h + 0.01f, 0, 0, 0, 150);
  text(x0, y0, v.over ? "DARTS 301 - FINAL" : "DARTS 301 (double out)", 255, 255, 255, 0.30f);
  float y = y0 + row;
  for (const View::Row& r : v.rows) {
    const bool cur = r.id == v.cur_id && !v.over;
    const bool won = v.over && r.id == v.winner_id;
    text(x0, y, std::format("{} {:<10.10}  {:>3}", cur ? ">" : (won ? "*" : " "), r.name, r.score),
         (cur || won) ? 255 : 230, cur ? 215 : (won ? 255 : 230), cur ? 70 : (won ? 120 : 230), 0.27f);
    y += row;
  }
  if (v.over) {
    for (const View::Row& r : v.rows)
      if (r.id == v.winner_id) text(x0, y + 0.004f, std::format("{} wins!", r.name), 120, 255, 120, 0.28f);
  }
}

// ---- host engine

void start_game(const Frame& f, const std::vector<const Player*>& partners) {
  g_humans.clear();
  Human host;
  host.id = f.my_id, host.name = name_of(f, f.my_id);
  g_humans.push_back(host);
  for (const Player* p : partners) {
    if (static_cast<int>(g_humans.size()) >= kMaxHumans) break;
    Human h;
    h.id = p->id, h.name = p->name.empty() ? std::format("Player {}", p->id) : p->name, h.ped = p->ped;
    g_humans.push_back(h);
  }
  g_my_id = f.my_id;
  g_cur = 0;
  g_over = false;
  g_winner = 0;
  g_fix_dart = false;
  g_prev_seat = g_prev_state = -1;
  fresh_players();
  for (const Human& h : g_humans)
    if (h.id != f.my_id) activity_seats::seat(h.id, h.ped, "darts", -1);
  activity_seats::set_active(kNobody);
  g_mode = Mode::Playing;
  std::string who;
  for (const Human& h : g_humans) who += " " + h.name;
  log::info("Darts: game started with {} humans:{}", g_humans.size(), who);
}

// Whoever has left: skipped in the rotation; if it's their turn the host throws for them.
void check_present(const Frame& f) {
  for (Human& h : g_humans) {
    if (h.gone || h.id == f.my_id) continue;
    const auto it = std::find_if(f.players.begin(), f.players.end(), [&](const Player& p) { return p.id == h.id; });
    if (it == f.players.end() || !it->ped || !n::DoesCharExist(it->ped)) {
      h.gone = true;
      activity_seats::unseat(h.id);
      log::info("Darts: {} left the game", h.name);
    } else {
      h.ped = it->ped;
    }
  }
}

void apply_active() {
  const Human& h = g_humans[g_cur];
  activity_seats::set_active(h.gone ? kNobody : id_for_active(h));
}

// The script has just handed the turn to seat 1 (sub_32182): give it to the next human on seat 0.
void rotate() {
  read_window(g_humans[g_cur]);
  std::size_t next = g_cur;
  for (std::size_t k = 1; k <= g_humans.size(); ++k) {
    const std::size_t c = (g_cur + k) % g_humans.size();
    if (!g_humans[c].gone) {
      next = c;
      break;
    }
  }
  g_cur = next;
  write_window(g_humans[g_cur]);
  st(g_lay.seat) = 0;
  st(g_lay.dart) = 0;
  st(g_lay.points) = 0;
  st(g_lay.hint) = 0;
  st(g_lay.sub) = 0;
  g_fix_dart = true;
  n::ClearHelp(); // HELP_SKIP of the AI's turn
  apply_active();
  log::debug("Darts: {} to throw ({} to go)", g_humans[g_cur].name, g_humans[g_cur].score);
}

void host_logic(const Frame& f) {
  const int state = st(g_lay.state);
  // A new game on the same script (the rematch resets both scores in the script): everyone starts again.
  if (g_prev_state == 2 && state != 2) {
    for (Human& h : g_humans) h.gone = false;
    g_cur = 0;
    g_over = false;
    g_winner = 0;
    fresh_players();
    write_window(g_humans[0]);
    g_prev_seat = -1;
    activity_seats::set_active(kNobody);
    log::info("Darts: new game");
  }
  g_prev_state = state;
  if (state == 2) {
    if (!g_over) {
      g_over = true;
      Human& cur = g_humans[g_cur];
      if (score(0) == 0) {
        g_winner = cur.id;
        log::info("Darts: {} wins", cur.name);
        phone::notify(std::format("{} wins the darts game", cur.name));
      }
      read_window(cur);
      activity_seats::set_active(kNobody); // the host answers "another game?"
    }
    return;
  }
  if (state != 1) return;
  check_present(f);
  const int seat = st(g_lay.seat);
  if (g_prev_seat == 0 && seat == 1) rotate();
  g_prev_seat = st(g_lay.seat);
  if (g_fix_dart) {
    if (st(g_lay.sub) != 0) g_fix_dart = false; // a dart is on its way: the count is the script's again
    else if (st(g_lay.dart) != 0) st(g_lay.dart) = 0; // bust path: sub_26709 counts a dart after the turn ended
  }
  // A partner who left on their turn: the host's player throws for them.
  if (g_humans[g_cur].gone) apply_active();
}

} // namespace

void register_hooks() {}

void host_tick(const Frame& f) {
  g_host_tick_at = GetTickCount();
  g_my_id = f.my_id;
  std::int32_t* st_now = activity_seats::statics("darts");
  if (!st_now) {
    if (g_mode != Mode::Idle) drop_game("script ended");
    g_mode = Mode::Idle;
    g_st = nullptr;
    draw(g_view);
    return;
  }
  if (st_now != g_st) { // a game just started (or another instance)
    if (g_mode == Mode::Playing) drop_game("script restarted");
    g_st = st_now;
    g_mode = Mode::Vanilla;
    if (!pick_layout()) {
      g_st = nullptr; // episode not known yet: look again next frame
      g_mode = Mode::Idle;
      return;
    }
    std::string why;
    const int words = std::max({g_lay.state, g_lay.sub, g_lay.seat, g_lay.dart, g_lay.points, g_lay.hint,
                                g_lay.hist + 16, g_lay.score + 3, g_lay.turn_start + 3}) + 1;
    if (!readable_range(g_st, static_cast<std::size_t>(words) * sizeof(std::int32_t))) {
      log::warn("Darts: the script's statics are not readable; vanilla 1v1");
      return;
    }
    if (!layout_ok(why)) {
      log::warn("Darts: the script's statics don't match the expected layout ({}); vanilla 1v1", why);
      return;
    }
    // Only a game seen at its very start is taken over (setup, or play with both scores untouched).
    const bool fresh = st(g_lay.state) == 0 ||
                       (st(g_lay.state) == 1 && score(0) == kStart && score(1) == kStart && st(g_lay.seat) == 0);
    const auto partners = partners_near(f);
    if (!fresh || partners.empty()) {
      log::info("Darts: {}; vanilla 1v1", fresh ? "nobody else at the board" : "the game was already under way");
      return;
    }
    start_game(f, partners);
  }
  if (g_mode == Mode::Playing) {
    std::string why;
    if (!layout_ok(why)) {
      log::warn("Darts: unexpected statics mid-game ({}); leaving the game to the script", why);
      drop_game("layout");
      g_mode = Mode::Vanilla;
      return;
    }
    host_logic(f);
    if (g_mode == Mode::Playing && !g_humans.empty()) rebuild_view();
  }
  draw(g_view);
}

// Host: send the board when it changed (and now and then, for the ones who join or miss one).
void host_send(net::Session& net) {
  const DWORD now = GetTickCount();
  if (!g_dirty && now - g_last_send < kResendMs) return;
  g_dirty = false;
  g_last_send = now;
  net::Writer w;
  w.put(kMsg).put(g_my_id).put(++g_seq).put(static_cast<std::uint8_t>(g_view.active ? 1 : 0))
      .put(static_cast<std::uint8_t>(g_view.over ? 1 : 0)).put(g_view.cur_id).put(g_view.winner_id)
      .put(static_cast<std::uint8_t>(g_view.rows.size()));
  for (const View::Row& r : g_view.rows) w.put(r.id).put(net::make_name(r.name)).put(r.score);
  net.send_game(std::move(w.data()), true);
}

// Partners: draw the host's board. Game thread, script context, every frame.
void client_tick() {
  if (GetTickCount() - g_host_tick_at < 1000) return; // we are the host: host_tick draws
  if (g_rx_valid && GetTickCount() - g_rx_at > kStaleMs) g_rx_valid = false, g_rx = View{};
  if (g_rx_valid) draw(g_rx);
}

void on_message(const net::GameMessage& m) {
  if (m.from != net::kHostId) return;
  net::Reader r(m.data);
  std::uint8_t type, sender, seq, active, over, cur, winner, count;
  if (!r.get(type) || !r.get(sender) || !r.get(seq) || !r.get(active) || !r.get(over) || !r.get(cur) ||
      !r.get(winner) || !r.get(count) || count > kMaxHumans)
    return;
  if (g_rx_valid && static_cast<std::int8_t>(seq - g_rx_seq) <= 0) return; // old or repeated
  View v;
  v.active = active != 0, v.over = over != 0, v.cur_id = cur, v.winner_id = winner;
  for (int i = 0; i < count; ++i) {
    View::Row row;
    net::Name name{};
    if (!r.get(row.id) || !r.get(name) || !r.get(row.score)) return;
    row.name = net::to_string(name);
    v.rows.push_back(std::move(row));
  }
  g_rx_seq = seq, g_rx_at = GetTickCount();
  g_rx_valid = v.active;
  g_rx = v.active ? std::move(v) : View{};
}

} // namespace ivc::activities::darts
