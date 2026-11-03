#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/episode.h"
#include "game/native_hooks.h"
#include "game/phone.h"
#include "sync/activities/activities.h"
#include "sync/activity_seats.h"

// TBoGT golf (docs/activities/golf.md). The script is a 10-shot target range with two sides, P (the player, the only
// side that reads input) and O (an AI opponent). Every human takes the P side in turn:
//   - the script's GET_PLAYER_CHAR returns the human whose turn it is (activity_seats::set_active), so the tee
//     attachment, the swing animations, the hand-overs and the exit all follow them with no static writes;
//   - the script's own per-side arrays are the active human's scorebook: before each human's turn their shots left
//     (l_U2884) and score (l_U2887) are written into side P's slots and read back when the shot is over;
//   - the O half-turn between two shots is bounced through the script's own skip path (IS_CONTROL_JUST_PRESSED(2, 77)
//     after 1 s, which resolves a made-up O shot and hands the tee back), and the human changes during it;
//   - the club (l_U3100) was attached to the first P ped at creation, so it is re-attached to each new golfer.
// All statics are checked against the layout in the research before anything is written; if they don't match, golf
// stays vanilla.
namespace ivc::activities::golf {
namespace {
namespace n = natives;
using native_hooks::Call;

constexpr std::uint32_t kIsControlJustPressed = 0x4CB729F1, kGetPlayerChar = 0x511454A9, kAttachObjectToPed = 0x577A699E,
                        kSetPlayerControl = 0x1A6203EA, kSetTextScale = 0x02C069E5, kSetTextColour = 0x19C967B5,
                        kSetTextDropshadow = 0x58F5023F, kSetTextFont = 0x75363BB5, kSetTextWrap = 0x19D006EB,
                        kSetTextProportional = 0x15585A65, kSetTextBackground = 0x768F5140,
                        kDisplayLiteral = 0x661B239A, kDrawRect = 0x3B2526E3;

// golf.txt statics (l_U<slot>). An array's size word is at its slot, element i at slot + 1 + i.
constexpr int kTotalShots = 2882;   // l_U2882 = 10 (:109)
constexpr int kShotsSize = 2883;    // l_U2883[2] shots left {P, O}; P 2884, O 2885 (:699-700, decrement :2376 :2751)
constexpr int kShotsP = 2884, kShotsO = 2885;
constexpr int kScoreSize = 2886;    // l_U2886[2] score {P, O}; P 2887, O 2888 (:701-702, += :2427 :2647)
constexpr int kScoreP = 2887, kScoreO = 2888;
constexpr int kTurn = 2890;         // 0 = P, 1 = O (:2765, :2788)
constexpr int kMode = 3082;         // 0/1 friend, 2 ambient (:194)
constexpr int kSkipClock = 3081;    // game timer at the P -> O hand-over (:2766); the skip needs > 1000 ms (:2344)
constexpr int kClub = 3100;         // the P club object (:661-666)
constexpr int kMain = 3136;         // 0 init, 1 setup, 2 intro, 3 play, 4 shot, 6 over, 7 quit prompt, 8 exit (:198-251)
constexpr int kPhase = 3138;        // per-shot phase, 0 = aim (:2552-2900)
constexpr int kSwing = 3140;        // swing state, 0 = idle/aim
constexpr int kStaticsWords = 3150; // l_U3143 arguments end at 3145

constexpr int kClubBone = 1219;     // ATTACH_OBJECT_TO_PED(l_U3100, ped, 1219, l_U2861, l_U2864, 0) (:666)
constexpr float kClubOff[3] = {0.03f, 0.017f, 0.0f}, kClubRot[3] = {4.75f, 3.0f, 1.25f};
constexpr float kWaitHeading = 120.7971f; // l_U2879
// Where the humans who aren't shooting wait: l_U2855 (the script's own waiting spot), l_U2858 (the bench) and two
// more beside them (golf.txt :94-95).
constexpr n::Vector3 kSpots[4] = {{-637.8589f, -12.0238f, 11.4022f},
                                  {-637.0303f, -15.0412f, 11.4018f},
                                  {-637.4446f, -13.5325f, 11.4020f},
                                  {-638.7000f, -13.5000f, 11.4020f}};

constexpr DWORD kHandOverMs = 700;   // the script finishes parking the last shooter within this long of the turn flip
constexpr DWORD kSkipAfterMs = 1250; // game ms into the O half-turn before we answer the skip (script wants > 1000)
constexpr DWORD kEndHoldMs = 1500;   // partners stay seated this long after the script ends so the move reaches them
constexpr int kMaxHumans = 4;

struct Human {
  std::uint8_t id = 0;
  std::string name;
  n::Ped ped = 0; // partner's puppet; 0 for the host (read live)
  int shots = 10, score = 0;
};

enum class Mode { Idle, Pending, Running, Vanilla, Ending };

Mode g_mode = Mode::Idle;
std::uint8_t g_my_id = 0;
std::vector<Human> g_cands, g_humans;
int g_cur = 0;
int g_club_owner = -1, g_parked_for = -1;
bool g_over = false;
DWORD g_flip_at = 0, g_end_at = 0;
bool g_armed = false, g_applied = false, g_swap_pending = false;
std::uint8_t g_swap_to = 0xFF;
bool g_host_off = false;

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool from_golf(const Call& c) { return lower(c.script) == "golf"; }
bool active_here() { return episode::current() == episode::Id::TBoGT; }

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

// The statics look like TBoGT golf's: the layout words and ranges the research found. Only then is anything written.
bool layout_ok(const std::int32_t* st, std::string& why) {
  if (!readable_range(st, kStaticsWords * sizeof(std::int32_t))) return why = "statics unreadable", false;
  if (st[kTotalShots] != 10) return why = "shots per side (l_U2882)", false;
  if (st[kShotsSize] != 2 || st[kScoreSize] != 2) return why = "array size words", false;
  if (st[kShotsP] < 0 || st[kShotsP] > 10 || st[kShotsO] < 0 || st[kShotsO] > 10) return why = "shots left", false;
  if (st[kTurn] != 0 && st[kTurn] != 1) return why = "turn", false;
  if (st[kMain] < 0 || st[kMain] > 8) return why = "main state", false;
  if (st[kClub] == 0 && st[kMain] >= 3) return why = "club object", false;
  return true;
}

n::Ped ped_of(const Human& h) { return h.id == g_my_id ? host_ped() : h.ped; }
std::uint8_t seat_id(const Human& h) { return h.id == g_my_id ? 0xFF : h.id; }

// The next human with shots left after g_cur (g_cur itself last); -1 if nobody has any.
int next_index() {
  const int cnt = static_cast<int>(g_humans.size());
  for (int k = 1; k <= cnt; ++k) {
    const int i = (g_cur + k) % cnt;
    if (g_humans[static_cast<std::size_t>(i)].shots > 0) return i;
  }
  return -1;
}

bool others_have_shots(int idx) {
  for (int i = 0; i < static_cast<int>(g_humans.size()); ++i)
    if (i != idx && g_humans[static_cast<std::size_t>(i)].shots > 0) return true;
  return false;
}

// O's shots left must stay above 0 until the last human's last shot, or the script's "either side has shots" test
// (:2433, :2759) ends the game early. Zero then makes the normal game-over path run.
int o_shots_for(int idx) {
  const Human& h = g_humans[static_cast<std::size_t>(idx)];
  return others_have_shots(idx) || h.shots > 1 ? 5 : 0;
}

int best_other_score(int idx) {
  int best = 0;
  for (int i = 0; i < static_cast<int>(g_humans.size()); ++i)
    if (i != idx) best = std::max(best, g_humans[static_cast<std::size_t>(i)].score);
  return best;
}

void sync_book(const std::int32_t* st) {
  Human& h = g_humans[static_cast<std::size_t>(g_cur)];
  h.shots = st[kShotsP];
  h.score = st[kScoreP];
}

// ---- hooks

// O half-turn: the script's skip answers (2, 77): once the host-side timing says it will take it, do it, and change
// the golfer for the turn that follows.
void on_control(const Call& c) {
  if (g_mode != Mode::Running || !g_armed || !from_golf(c)) return;
  if (c.arg<std::int32_t>(0) != 2 || c.arg<std::int32_t>(1) != 77) return;
  std::int32_t* st = activity_seats::statics("golf");
  if (!st || st[kTurn] != 1) return;
  if (!g_applied) {
    const int nx = next_index();
    if (nx < 0) return;
    sync_book(st);
    const Human& h = g_humans[static_cast<std::size_t>(nx)];
    st[kShotsP] = h.shots;
    st[kScoreP] = h.score;
    st[kShotsO] = 5;
    g_swap_to = seat_id(h);
    g_swap_pending = true;
    g_applied = true;
    log::info("Golf: {} is up next ({} shots left, score {})", h.name, h.shots, h.score);
    g_cur = nx;
  }
  *reinterpret_cast<std::int32_t*>(c.ctx->result) = 1;
}

// The first GET_PLAYER_CHAR after the skip is the script handing the tee to the new golfer: that call and the ones
// after it answer with them.
void before_player_char(const Call& c) {
  if (!g_swap_pending || !from_golf(c)) return;
  g_swap_pending = false;
  activity_seats::set_active(g_swap_to);
}

// ---- actions

void attach_club(const std::int32_t* st, n::Ped ped) {
  if (!ped || !st[kClub]) return;
  n::invoke(kAttachObjectToPed, st[kClub], ped, kClubBone, kClubOff[0], kClubOff[1], kClubOff[2], kClubRot[0],
            kClubRot[1], kClubRot[2], 0);
}

void park_others() {
  for (int i = 0; i < static_cast<int>(g_humans.size()); ++i) {
    if (i == g_cur) continue;
    const n::Ped p = ped_of(g_humans[static_cast<std::size_t>(i)]);
    if (!p || !n::DoesCharExist(p)) continue;
    n::SetCharCoordinatesNoOffset(p, kSpots[i % 4]);
    n::SetCharHeading(p, kWaitHeading);
  }
}

void host_control(bool on) {
  if (on == !g_host_off) return;
  g_host_off = !on;
  n::invoke(kSetPlayerControl, n::GetPlayerId(), on);
}

void reset_round() {
  const int total = 10;
  for (Human& h : g_humans) h.shots = total, h.score = 0;
  g_cur = 0;
  g_club_owner = g_parked_for = -1;
  g_over = false;
  g_applied = g_swap_pending = g_armed = false;
  activity_seats::set_active(0xFF);
  log::info("Golf: rematch, scorebooks reset");
}

void announce() {
  int top = -1;
  for (const Human& h : g_humans) top = std::max(top, h.score);
  std::string line = "Golf: ", winners;
  int wn = 0;
  for (const Human& h : g_humans) {
    line += std::format("{} {}  ", h.name, h.score);
    if (h.score == top) winners += (wn++ ? ", " : "") + h.name;
  }
  line += wn == 1 ? std::format("- {} wins!", winners) : std::format("- tie: {}", winners);
  log::info("{}", line);
  phone::notify(line);
}

void finish_round(const std::int32_t* st) {
  sync_book(st);
  g_over = true;
  announce();
}

void stop_managing(const char* why, bool keep_seats = false) {
  log::warn("Golf: {}; the rest is vanilla", why);
  if (!keep_seats)
    for (const Human& h : g_humans)
      if (h.id != g_my_id) activity_seats::unseat(h.id);
  activity_seats::set_active(0xFF);
  host_control(true);
  g_swap_pending = g_armed = false;
  g_mode = Mode::Vanilla;
}

// ---- scoreboard (host only; partners seeing it comes with a golf message later)

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

void draw(int total) {
  constexpr float x0 = 0.03f, y0 = 0.30f, row = 0.034f;
  const float h = row * static_cast<float>(g_humans.size() + 1);
  n::invoke(kDrawRect, 0.17f, y0 + h * 0.5f - 0.01f, 0.31f, h + 0.01f, 0, 0, 0, 150);
  text(x0, y0, g_over ? std::string("GOLF - FINAL    shots  score") : std::string("GOLF            shots  score"), 255, 255,
       255, 0.30f);
  float y = y0 + row;
  for (int i = 0; i < static_cast<int>(g_humans.size()); ++i) {
    const Human& hu = g_humans[static_cast<std::size_t>(i)];
    const bool cur = i == g_cur && !g_over;
    text(x0, y, std::format("{} {:<10.10}  {:>2}/{:<2}   {:>3}", cur ? ">" : " ", hu.name, total - hu.shots, total, hu.score),
         cur ? 255 : 230, cur ? 215 : 230, cur ? 70 : 230, 0.27f);
    y += row;
  }
}

// ---- per frame

void begin(std::int32_t* st, const Frame& f) {
  g_humans.clear();
  for (const Player& p : f.players)
    if (p.id == f.my_id) g_humans.push_back({p.id, p.name, 0, st[kTotalShots], 0});
  for (const Human& c : g_cands) {
    if (!c.ped || !n::DoesCharExist(c.ped) || static_cast<int>(g_humans.size()) >= kMaxHumans) continue;
    g_humans.push_back(c);
    g_humans.back().shots = st[kTotalShots];
  }
  if (g_humans.size() < 2) return stop_managing("nobody to play with any more", true);
  for (const Human& h : g_humans)
    if (h.id != g_my_id) activity_seats::seat(h.id, h.ped, "golf", -1);
  activity_seats::set_active(0xFF);
  g_cur = 0, g_club_owner = 0, g_parked_for = -1, g_over = false, g_applied = g_swap_pending = g_armed = false;
  g_flip_at = 0;
  g_mode = Mode::Running;
  std::string who;
  for (const Human& h : g_humans) who += " " + h.name;
  log::info("Golf: {} golfers:{}", g_humans.size(), who);
}

void end_game(const Frame& f) {
  (void)f;
  g_mode = Mode::Ending;
  g_end_at = GetTickCount();
  activity_seats::set_active(0xFF);
  host_control(true);
  g_swap_pending = g_armed = false;
  const n::Ped me = host_ped();
  if (me) {
    const n::Vector3 p = n::GetCharCoordinates(me);
    int k = 0;
    for (const Human& h : g_humans) {
      if (h.id == g_my_id || !h.ped || !n::DoesCharExist(h.ped)) continue;
      ++k;
      n::SetCharCoordinatesNoOffset(h.ped, {p.x + 0.9f * static_cast<float>(k), p.y, p.z});
    }
  }
  log::info("Golf: over, golfers sent back to the exit");
}

void run(std::int32_t* st, const Frame& f) {
  std::string why;
  if (!layout_ok(st, why)) return stop_managing(("layout changed (" + why + ")").c_str());
  // Partners who left (puppet gone / seat dropped) or whose puppet was recreated.
  for (int i = 0; i < static_cast<int>(g_humans.size());) {
    Human& h = g_humans[static_cast<std::size_t>(i)];
    if (h.id == g_my_id) {
      ++i;
      continue;
    }
    n::Ped now = 0;
    for (const Player& p : f.players)
      if (p.id == h.id) now = p.ped;
    if (!now || !n::DoesCharExist(now) || !activity_seats::seated(h.id)) {
      if (i == g_cur) return stop_managing(std::format("{} left during their turn", h.name).c_str());
      log::info("Golf: {} left", h.name);
      activity_seats::unseat(h.id);
      g_humans.erase(g_humans.begin() + i);
      if (i < g_cur) --g_cur;
      g_club_owner = g_parked_for = -1;
      continue;
    }
    if (now != h.ped) h.ped = now, activity_seats::seat(h.id, now, "golf", -1);
    ++i;
  }
  if (g_humans.size() < 2) return stop_managing("only one golfer left", true);

  const std::int32_t main_state = st[kMain], turn = st[kTurn];
  if (main_state == 1 && g_over) reset_round();
  if (main_state == 8) { // leaving: the host is the one the exit moves
    activity_seats::set_active(0xFF);
    host_control(true);
    return;
  }
  if (main_state == 6) { // game over: the host decides rematch / quit
    if (!g_over) finish_round(st);
    activity_seats::set_active(0xFF);
    host_control(true);
    draw(st[kTotalShots]);
    return;
  }
  if (main_state < 3 || main_state == 5) return;

  const bool stable = main_state == 3 && st[kPhase] == 0 && st[kSwing] == 0;
  if (turn == 0) {
    g_flip_at = 0, g_applied = false, g_armed = false;
    sync_book(st);
    st[kShotsO] = o_shots_for(g_cur);
    st[kScoreO] = best_other_score(g_cur);
    if (stable && g_club_owner != g_cur) {
      const n::Ped p = ped_of(g_humans[static_cast<std::size_t>(g_cur)]);
      attach_club(st, p);
      if (p && g_cur != 0) {
        n::SetBlockingOfNonTemporaryEvents(p, true);
        n::SetCurrentCharWeapon(p, 0);
      }
      g_club_owner = g_cur;
    }
    if (stable && g_parked_for != g_cur) {
      park_others();
      g_parked_for = g_cur;
    }
    // The script gives the host control at the start of every turn; keep them out of it while someone else shoots.
    host_control(g_humans[static_cast<std::size_t>(g_cur)].id == g_my_id);
  } else {
    const DWORD now = GetTickCount();
    if (!g_flip_at) g_flip_at = now;
    if (!g_applied) sync_book(st);
    // Once the script has parked the last shooter (a few frames), the O half-turn is nobody's: nothing of a partner's
    // input should answer its reads.
    if (!g_applied && now - g_flip_at > kHandOverMs) activity_seats::set_active(0xFF);
    const std::int32_t clock = static_cast<std::int32_t>(n::GetGameTimer()) - st[kSkipClock];
    g_armed = main_state == 3 && st[kPhase] == 0 && st[kSwing] == 0 && clock > static_cast<std::int32_t>(kSkipAfterMs) &&
              now - g_flip_at > kHandOverMs && next_index() >= 0;
  }
  draw(st[kTotalShots]);
}

} // namespace

void register_hooks() {
  native_hooks::on(kIsControlJustPressed, "IS_CONTROL_JUST_PRESSED", on_control);
  native_hooks::before(kGetPlayerChar, "GET_PLAYER_CHAR", before_player_char);
}

void host_tick(const Frame& f) {
  g_my_id = f.my_id;
  std::int32_t* st = active_here() ? activity_seats::statics("golf") : nullptr;
  if (g_mode == Mode::Ending) {
    if (GetTickCount() - g_end_at > kEndHoldMs) {
      for (const Human& h : g_humans)
        if (h.id != g_my_id) activity_seats::unseat(h.id);
      g_humans.clear(), g_cands.clear();
      g_mode = Mode::Idle;
    }
    return;
  }
  if (!st) {
    if (g_mode == Mode::Running) return end_game(f);
    if (g_mode == Mode::Vanilla || g_mode == Mode::Pending) g_mode = Mode::Idle, g_cands.clear();
    return;
  }
  switch (g_mode) {
    case Mode::Idle: {
      // The script just started: whoever is at the range now plays (partners join at the tee when it sets up).
      g_cands.clear();
      for (const Player* p : partners_near(f)) g_cands.push_back({p->id, p->name, p->ped, 10, 0});
      g_mode = g_cands.empty() ? Mode::Vanilla : Mode::Pending;
      log::info("Golf: started, {} partner(s) near", g_cands.size());
      return;
    }
    case Mode::Pending: {
      if (!readable_range(st, kStaticsWords * sizeof(std::int32_t))) return;
      if (st[kMain] < 3) return; // set up and introduced first
      std::string why;
      if (!layout_ok(st, why)) {
        log::warn("Golf: statics don't match the expected layout ({}); vanilla", why);
        g_mode = Mode::Vanilla;
        return;
      }
      begin(st, f);
      return;
    }
    case Mode::Running:
      run(st, f);
      return;
    default:
      return;
  }
}

} // namespace ivc::activities::golf
