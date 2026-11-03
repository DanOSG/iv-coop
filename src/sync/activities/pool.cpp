#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/episode.h"
#include "game/phone.h"
#include "sync/activities/activities.h"
#include "sync/activity_seats.h"

// Pool, 1v1: a partner takes pool_game's opponent seat (seat 0) instead of the AI/friend; the host keeps seat 1 (the
// script's player seat). Others at the table queue for it; the winner stays on. See docs/activities/pool.md.
namespace ivc::activities::pool {
namespace {
namespace n = natives;

// pool_game's statics per game (research/GTAIVMissionScripts/{IV,TLAD,TBOGT}/pool_game.txt; array `l_Un[k]` is word
// n + 1 + k, word n is its size). The block pool_table passes is 11 words, laid out
// {ped[2], player[2], table, flag, pad[2]} each array with a size word: ped0 = copy+1, ped1 = copy+2, pad0 = copy+9.
//  - args / copy: IV `l_U318 = {l_U750};` (:120), TLAD and TBOGT `l_U285 = {l_U722};` (:81 in both). The block is
//    started from pool_table with `START_NEW_SCRIPT_WITH_ARGS("pool_game", ref l_U19, 11, 1024)` (IV :309, TLAD
//    :581 with l_U490, TBOGT :464 with l_U301), so the args words sit at 750.. / 722.. and the live copy at 318.. / 285..
//  - human flags: IV `l_U56[0] = 0; l_U56[1] = 1;` (:310-311) -> words 57/58; TLAD and TBOGT `l_U58[0] = 0;
//    l_U58[1] = 1;` (:200-201 in both) -> words 59/60. The only writes to the flag array in the script.
//  - turn: IV l_U243 (set to 1 :378, swapped :1701/1705, :2134), TLAD/TBOGT l_U203 (:375, :1698/1702).
//  - state: IV l_U240 (`l_U240 = iParam0;` in sub_9759 :3306), TLAD l_U200 (:3264), TBOGT l_U200 (:3278); 0 = init,
//    7 = game over (state machine of docs/activities/pool.md section 3).
//  - winner: IV l_U264 (-1 at :86, `= l_U243` :1844/:2039, `= l_U244` :2129), TLAD/TBOGT l_U224 (-1 at :46, :1841).
//  - statics_words: highest slot + 1 (the args block ends at 760 / 732).
struct Layout {
  int args, copy, flags, turn, state, winner, statics_words;
};
constexpr Layout kLayoutIV{750, 318, 56, 243, 240, 264, 761};
constexpr Layout kLayoutTLAD{722, 285, 58, 203, 200, 224, 733};
constexpr Layout kLayoutTBoGT{722, 285, 58, 203, 200, 224, 733};
constexpr std::int32_t kStateOver = 7;
constexpr std::int32_t kPadWord = 1; // seat 0's pad index; the host's seat reads pad 0, so the sticks are separable

struct Game {
  std::int32_t* st = nullptr; // the thread's statics (identity of the running game)
  bool done_setup = false;    // our writes are in (once per game)
  bool gave_up = false;       // too late / layout doubt: single player for this game
  std::uint8_t partner = 0;
  n::Ped puppet = 0, original = 0;
  bool result_seen = false;
  int last_winner = -1; // 0 = partner (seat 0), 1 = host
};

Game g_game;
Layout g_lay{};
std::vector<std::uint8_t> g_queue; // waiting challengers, longest wait first (the seated one is not in it)

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

const Player* find_player(const Frame& f, std::uint8_t id) {
  for (const Player& p : f.players)
    if (p.id == id) return &p;
  return nullptr;
}

std::string host_name(const Frame& f) {
  const Player* me = find_player(f, f.my_id);
  return me && !me->name.empty() ? me->name : "Host";
}

// Keeps g_queue = the partners at the table (minus the seated one), longest waiting first.
void update_queue(const Frame& f) {
  const auto near_ = partners_near(f);
  const auto here = [&](std::uint8_t id) {
    return std::any_of(near_.begin(), near_.end(), [&](const Player* p) { return p->id == id; });
  };
  const bool seated = g_game.done_setup;
  std::erase_if(g_queue, [&](std::uint8_t id) { return !here(id) || (seated && id == g_game.partner); });
  for (const Player* p : near_) {
    if (seated && p->id == g_game.partner) continue;
    if (std::find(g_queue.begin(), g_queue.end(), p->id) == g_queue.end()) g_queue.push_back(p->id);
  }
}

// Undo our writes on the (possibly still running) script and drop the seat.
void release(bool script_alive) {
  if (!g_game.done_setup) return;
  activity_seats::set_active(0xFF);
  activity_seats::unseat(g_game.partner);
  if (script_alive && g_game.st && readable_range(g_game.st, g_lay.statics_words * sizeof(std::int32_t))) {
    std::int32_t* st = g_game.st;
    if (st[g_lay.copy + 1] == static_cast<std::int32_t>(g_game.puppet)) st[g_lay.copy + 1] = g_game.original;
    st[g_lay.flags + 1] = 0; // the AI plays seat 0 again
  }
  if (g_game.original && n::DoesCharExist(g_game.original)) {
    n::FreezeCharPosition(g_game.original, false);
    n::SetCharVisible(g_game.original, true);
  }
}

void end_game(bool script_alive) {
  if (g_game.done_setup) {
    // The winner stays on: a partner who won keeps seat 0 next game, a loser queues behind whoever has waited.
    const std::uint8_t p = g_game.partner;
    release(script_alive);
    if (g_game.last_winner == 0) g_queue.insert(g_queue.begin(), p);
    else g_queue.push_back(p);
  }
  g_game = Game{};
}

// Seats a challenger into the script that is in its setup state.
void try_setup(const Frame& f, std::int32_t* st) {
  if (!readable_range(st, g_lay.statics_words * sizeof(std::int32_t))) {
    log::warn("Pool: statics of pool_game not readable, single player");
    g_game.gave_up = true;
    return;
  }
  const bool copied = st[g_lay.copy] == 2; // the script has run its first statement (`l_U318 = {l_U750}`)
  const int base = copied ? g_lay.copy : g_lay.args;
  if (st[g_lay.state] != 0 || (copied && st[g_lay.state + 1] != 0)) {
    log::warn("Pool: pool_game already past setup (state {}), single player this game", st[g_lay.state]);
    g_game.gave_up = true;
    return;
  }
  // The block as it is now: size words 2, seat 0's ped (the friend/stranger), seat 1 = the host's player.
  const n::Ped host = host_ped();
  const n::Ped ped0 = static_cast<n::Ped>(st[base + 1]), ped1 = static_cast<n::Ped>(st[base + 2]);
  std::string why;
  if (st[base] != 2 || st[base + 3] != 2 || st[base + 8] != 2) why = "block size words";
  else if (!host || ped1 != host) why = "seat 1 is not the host's ped";
  else if (!ped0 || ped0 == host || !n::DoesCharExist(ped0)) why = "seat 0 ped not a live ped";
  else if (copied) {
    const std::int32_t turn = st[g_lay.turn], win = st[g_lay.winner];
    if (st[g_lay.flags] != 2) why = "human flag array size";
    else if (st[g_lay.flags + 1] != 0 && st[g_lay.flags + 1] != 1) why = "human flag 0";
    else if (st[g_lay.flags + 2] != 0 && st[g_lay.flags + 2] != 1) why = "human flag 1";
    else if (turn != 0 && turn != 1) why = "turn word";
    else if (win < -1 || win > 1) why = "winner word";
  }
  if (!why.empty()) {
    log::warn("Pool: pool_game statics don't look right ({}), single player this game", why);
    g_game.gave_up = true;
    return;
  }

  // The challenger: first queued partner whose puppet is here.
  const Player* who = nullptr;
  for (std::uint8_t id : g_queue)
    if (const Player* p = find_player(f, id); p && p->ped && n::DoesCharExist(p->ped)) {
      who = p;
      break;
    }
  if (!who) return; // nobody at the table yet; the setup state lasts a while, try again next frame

  // The puppet replaces seat 0 in the live copy (or, before the copy, in the args it will copy), marked human with
  // its own pad word. pool_table's own block is untouched, so its cleanup deletes the original ped, not the puppet.
  st[base + 1] = static_cast<std::int32_t>(who->ped);
  st[base + 9] = kPadWord;
  if (copied) st[g_lay.flags + 1] = 1;
  g_game.done_setup = true;
  g_game.partner = who->id;
  g_game.puppet = who->ped;
  g_game.original = ped0;
  g_game.result_seen = false;
  g_game.last_winner = -1;
  std::erase(g_queue, who->id);

  n::SetCharVisible(ped0, false);
  n::FreezeCharPosition(ped0, true);
  activity_seats::seat(who->id, who->ped, "pool_game", kPadWord);
  activity_seats::set_stand_in(who->id, ped0);
  log::info("Pool: {} takes seat 0 against {} ({}, original opponent ped {})", who->name, host_name(f),
            copied ? "live copy" : "args", ped0);
  phone::notify(std::format("{} is up against you at pool.", who->name));
}

} // namespace

void register_hooks() {}

void host_tick(const Frame& f) {
  if (!pick_layout()) return;
  std::int32_t* st = activity_seats::statics("pool_game");
  if (!st) {
    if (g_game.st || g_game.done_setup) end_game(false);
    update_queue(f);
    return;
  }
  if (st != g_game.st) {
    end_game(false); // a different run than the one we seated into (its statics are gone)
    g_game.st = st;
  }
  update_queue(f);
  if (g_game.gave_up) return;
  if (!g_game.done_setup) {
    try_setup(f, st);
    return;
  }

  // Seated: keep it consistent. A partner who left or whose puppet is gone hands the seat back to the AI.
  const Player* p = find_player(f, g_game.partner);
  if (!p || !p->ped || p->ped != g_game.puppet || !n::DoesCharExist(g_game.puppet)) {
    log::info("Pool: seated partner {} left, the AI takes seat 0 back", g_game.partner);
    release(true);
    g_game.gave_up = true;
    g_game.done_setup = false;
    return;
  }
  if (!readable_range(st, g_lay.statics_words * sizeof(std::int32_t))) return;
  if (st[g_lay.copy] == 2) { // the script has its copy; pin seat 0 as the human puppet with the separable pad
    st[g_lay.copy + 1] = static_cast<std::int32_t>(g_game.puppet);
    st[g_lay.copy + 9] = kPadWord;
    st[g_lay.flags + 1] = 1; // the script writes 0 here once in its init (state 0), then never again
  }
  const std::int32_t turn = st[g_lay.turn], state = st[g_lay.state], win = st[g_lay.winner];
  activity_seats::set_active(turn == 0 ? g_game.partner : 0xFF);

  if (win == -1) {
    g_game.result_seen = false; // rematch
  } else if (state == kStateOver && (win == 0 || win == 1) && !g_game.result_seen) {
    g_game.result_seen = true;
    g_game.last_winner = win;
    const std::string winner = win == 0 ? p->name : host_name(f);
    log::info("Pool: {} won ({} vs {})", winner, p->name, host_name(f));
    phone::notify(std::format("{} won at pool.", winner));
  }
}

} // namespace ivc::activities::pool
