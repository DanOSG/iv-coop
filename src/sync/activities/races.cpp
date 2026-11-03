#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/episode.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "game/phone.h"
#include "net/bytes.h"
#include "net/protocol.h"
#include "net/session.h"
#include "sync/activities/activities.h"
#include "sync/activities/races.h"
#include "sync/activity_seats.h"
#include "sync/remote_players.h"

// TBoGT street races (racesplauncher -> script raceSP) with partners as racers: each takes one of the three AI slots,
// races as their own character in their own car, from their own grid spot, and gets their own place at the end.
//
// How: the script builds the grid itself (3 AI cars + drivers in slots 0-2, the player in slot 3). When the grid is
// populated and the countdown starts, each partner (up to 3, those near the start) is told to build a car of that
// slot's model on the slot's grid spot and sit in it (message 79); the script's AI car and driver for that slot become
// that partner's "stand-in": hidden in the sky, frozen. Once the partner's puppet sits in the host's copy of their car
// (the normal vehicle sync), the slot's ped and car words in the script's statics are replaced by the puppet and the
// copy. From then on the script tracks that racer with its own code (checkpoint progress, laps, places), which works on
// the puppet's synced position. Everything the script would do *to* that slot (drive it, clear its tasks, warp it to a
// checkpoint to rubber-band, delete it at cleanup) is aimed at the stand-in by before-hooks (the activity_seats
// before_release pattern), so the partner's own driving is never fought. Racers are not seated (activity_seats):
// that would stop the puppet following the partner's driving. The host's script keeps the race; the mod only adds the
// grid message, a "go" at the start (partners' cars are held until then) and the result per partner.
//
// Only the car/bike race types of TBoGT `racesp` (type word 0, 1, 2) with the full 4 racers are taken over; every
// other layout, type or moment is left to the script (vanilla race) with a log line. TLAD RoadRash has 6 AI racers, a
// different racer struct and combat, so it is not handled here (docs/ARCHITECTURE.md "Races").
namespace ivc::activities::races {

namespace {
namespace n = natives;
using native_hooks::Call;

constexpr std::uint32_t kClearTasks = 0x4AB470F3, kClearTasksImmediately = 0x3C116620,
                        kTaskCarMissionCoorsTarget = 0x36D51DDF, kTaskCarMission = 0x36273536,
                        kTaskCarTempAction = 0x11612815, kTaskCarDriveWander = 0x1E9635A9,
                        kTaskPerformSequence = 0x36A33C21, kTaskLeaveAnyCar = 0x1114089D,
                        kTaskLeaveCarImmediately = 0x7BFB484F, kWarpCharIntoCar = 0x73D3504A,
                        kSetCharCoordinates = 0x689D0F5F, kSetCharHeading = 0x46B5523B,
                        kApplyForceToCar = 0x434611A3, kSetDriveTaskCruiseSpeed = 0x499700EF,
                        kDeleteChar = 0x0E3B49BF, kMarkCharNoLongerNeeded = 0x0B774604,
                        kRemoveCharElegantly = 0x5731084A, kDeleteCar = 0x7F71342D,
                        kMarkCarNoLongerNeeded = 0x20C76FD1, kSetCarHeading = 0x75E40528,
                        kSetCarOnGroundProperly = 0x0E717E98, kRequestCollisionAt = 0x12ED0BC9,
                        kLoadScene = 0x39F62BFB;

constexpr std::uint8_t kNobody = 0xFF;
constexpr int kMaxPartners = 3;
constexpr DWORD kSwapGraceMs = 5000;  // after the race starts, a partner not in their car by then is dropped
constexpr DWORD kHoldMaxMs = 45000;   // partner: the car is let go after this even without "go"
constexpr float kHideUp = 200.0f;     // stand-ins wait this far above their grid spot
constexpr float kCandidateRange = 40.0f;

// racesp's statics (TBoGT decompile, racesp.txt; l_U<n> = static n, word index). Race struct l_U877 (byte offsets
// /4): checkpoint count _fU824 = 1083, laps _fU828 = 1084, racer count _fU832 = 1085, type _fU852 = 1090. Stage l_U874
// (0 grid/countdown, 1 running, 2 cleanup; :331 `switch (l_U874)`), sub-stage l_U875 (5 = countdown, set :2269 right
// after the grid is populated, handled :259-270), player slot l_U1128 = 3 (:173, never written again), racer array
// l_U1129 (count word 4, then elements of `kStride` words, found by probing). Racer fields in words: ped 0, car 2,
// model 3 (_fU12), grid pos 9-11 (_fU36), heading 12 (_fU48), next checkpoint 13 (_fU52), lap 15 (_fU60), distance
// to it 17 (_fU68), status 18 (_fU72, 5 = finished: :6246 sets it after the last lap, :377 the player's win).
constexpr int kStage = 874, kSubStage = 875, kRaceCheckpoints = 1083, kRaceLaps = 1084, kRaceRacers = 1085,
              kRaceType = 1090, kPlayerSlotWord = 1128, kArrayCount = 1129;
constexpr int kFPed = 0, kFCar = 2, kFModel = 3, kFGrid = 9, kFHeading = 12, kFCheck = 13, kFLap = 15, kFDist = 17,
              kFStatus = 18;
constexpr int kPlayerSlot = 3, kSlots = 4;
constexpr std::int32_t kPlayerCarModel = 1063483177; // sub_24166 :2779, the player slot's _fU12

struct Rank {
  int slot;
  bool finished;
  int lap, check;
  float dist;
};

struct Racer { // a partner in an AI slot
  std::uint8_t id = 0;
  std::string name;
  int slot = 0;
  n::Ped stand_ped = 0;
  n::Vehicle stand_car = 0;
  n::Vector3 grid{};
  float heading = 0;
  std::int32_t model = 0;
  n::Ped puppet = 0;
  n::Vehicle car = 0;
  bool swapped = false, dropped = false;
};

enum class Mode { Idle, Vanilla, Waiting, Racing } g_mode = Mode::Idle;
std::int32_t* g_st = nullptr;
int g_base0 = 0, g_stride = 0; // racer array: first element's word, words per element
std::vector<Racer> g_racers;
std::vector<std::uint8_t> g_candidates;
bool g_go_sent = false, g_hit_logged = false;
DWORD g_run_at = 0;
std::vector<Rank> g_last; // newest standings, best first
bool g_last_valid = false;
int g_laps = 0, g_checks = 0;
std::uint8_t g_my_id = 0;
std::vector<std::vector<std::uint8_t>> g_out;
DWORD g_host_tick_at = 0;

// Partner.
struct Grid {
  std::uint8_t slot = 0;
  std::uint32_t model = 0;
  n::Vector3 at{};
  float heading = 0;
  DWORD since = 0;
  bool scene_requested = false;
};
bool g_have_grid = false;
Grid g_grid;
n::Vehicle g_my_car = 0;
bool g_holding = false;
DWORD g_hold_since = 0;
std::uint8_t g_client_id = 0;

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
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

std::int32_t& st(int word) { return g_st[word]; }
std::int32_t& field(int slot, int f) { return g_st[g_base0 + g_stride * slot + f]; }
float fld_float(int slot, int f) {
  float v;
  std::memcpy(&v, &field(slot, f), sizeof(v));
  return v;
}

n::Ped host_player() {
  const n::Ped p = n::GetPlayerChar(n::GetPlayerId());
  return p && n::DoesCharExist(p) ? p : 0;
}

net::Writer msg(std::uint8_t kind, std::uint8_t target) {
  net::Writer w;
  w.put(kMsg).put(g_my_id).put(kind).put(target);
  return w;
}

void queue(net::Writer&& w) {
  if (g_out.size() < 64) g_out.push_back(std::move(w.data()));
}

const char* ordinal(int p) {
  static const char* k[] = {"0th", "1st", "2nd", "3rd", "4th"};
  return p >= 1 && p <= 4 ? k[p] : "?";
}

// ---- the redirect: what the script does to a partner's racer goes to its stand-in

n::Ped stand_ped_for(n::Ped ped) {
  if (!ped) return 0;
  for (const Racer& r : g_racers)
    if (r.swapped && r.puppet == ped) return r.stand_ped;
  return 0;
}

n::Vehicle stand_car_for(n::Vehicle car) {
  if (!car) return 0;
  for (const Racer& r : g_racers)
    if (r.swapped && r.car == car) return r.stand_car;
  return 0;
}

struct Redirect {
  std::uint32_t hash;
  const char* name;
  int ped = -1;       // argument slot holding a ped (-1: none)
  int car = -1;       // argument slot holding a vehicle
  bool by_pointer = false; // the ped / vehicle argument is the script's variable (delete / release natives)
};
constexpr Redirect kRedirects[] = {
    {kClearTasks, "CLEAR_CHAR_TASKS", 0},
    {kClearTasksImmediately, "CLEAR_CHAR_TASKS_IMMEDIATELY", 0},
    {kTaskCarMissionCoorsTarget, "TASK_CAR_MISSION_COORS_TARGET", 0, 1},
    {kTaskCarMission, "TASK_CAR_MISSION", 0, 1},
    {kTaskCarTempAction, "TASK_CAR_TEMP_ACTION", 0, 1},
    {kTaskCarDriveWander, "TASK_CAR_DRIVE_WANDER", 0, 1},
    {kTaskPerformSequence, "TASK_PERFORM_SEQUENCE", 0},
    {kTaskLeaveAnyCar, "TASK_LEAVE_ANY_CAR", 0},
    {kTaskLeaveCarImmediately, "TASK_LEAVE_CAR_IMMEDIATELY", 0, 1},
    {kWarpCharIntoCar, "WARP_CHAR_INTO_CAR", 0, 1},
    {kSetCharCoordinates, "SET_CHAR_COORDINATES", 0},
    {kSetCharHeading, "SET_CHAR_HEADING", 0},
    {kApplyForceToCar, "APPLY_FORCE_TO_CAR", -1, 0},
    {kSetDriveTaskCruiseSpeed, "SET_DRIVE_TASK_CRUISE_SPEED", 0},
    {kDeleteChar, "DELETE_CHAR", 0, -1, true},
    {kMarkCharNoLongerNeeded, "MARK_CHAR_AS_NO_LONGER_NEEDED", 0, -1, true},
    {kRemoveCharElegantly, "REMOVE_CHAR_ELEGANTLY", 0},
    {kDeleteCar, "DELETE_CAR", -1, 0, true},
    {kMarkCarNoLongerNeeded, "MARK_CAR_AS_NO_LONGER_NEEDED", -1, 0, true},
};

void redirect(const Call& c, const Redirect& r) {
  if (g_mode != Mode::Racing || g_racers.empty() || lower(c.script) != "racesp") return;
  auto* args = static_cast<std::uint32_t*>(c.ctx->args);
  const auto swap_arg = [&](int idx, bool car) {
    if (idx < 0 || idx >= c.arg_count) return;
    if (r.by_pointer) {
      auto* var = reinterpret_cast<std::int32_t*>(static_cast<std::uintptr_t>(args[idx]));
      if (!var || IsBadReadPtr(var, 4)) return;
      const std::int32_t other = car ? stand_car_for(*var) : stand_ped_for(*var);
      if (other) *var = other;
    } else {
      const std::int32_t other = car ? stand_car_for(static_cast<std::int32_t>(args[idx]))
                                     : stand_ped_for(static_cast<std::int32_t>(args[idx]));
      if (other) args[idx] = static_cast<std::uint32_t>(other);
    }
  };
  swap_arg(r.ped, false);
  swap_arg(r.car, true);
}

// ---- stand-ins

void hide(Racer& r) {
  if (r.stand_car && n::DoesVehicleExist(r.stand_car)) {
    n::SetCarCoordinatesNoOffset(r.stand_car, {r.grid.x, r.grid.y, r.grid.z + kHideUp});
    n::FreezeCarPosition(r.stand_car, true);
    n::SetCarCollision(r.stand_car, false);
    n::SetCarCanBeDamaged(r.stand_car, false);
  }
  if (r.stand_ped && n::DoesCharExist(r.stand_ped)) n::SetCharInvincible(r.stand_ped, true);
}

void unhide(Racer& r) {
  if (r.stand_car && n::DoesVehicleExist(r.stand_car)) {
    n::SetCarCoordinatesNoOffset(r.stand_car, r.grid);
    n::invoke(kSetCarHeading, r.stand_car, r.heading);
    n::invoke(kSetCarOnGroundProperly, r.stand_car);
    n::SetCarCollision(r.stand_car, true);
    n::SetCarCanBeDamaged(r.stand_car, true);
    n::FreezeCarPosition(r.stand_car, false);
  }
  if (r.stand_ped && n::DoesCharExist(r.stand_ped)) n::SetCharInvincible(r.stand_ped, false);
}

// Slot goes back to its stand-in (the partner left, or never made it into a car): the script's AI race it again.
void release_slot(Racer& r, const char* why) {
  if (r.swapped && g_st) {
    field(r.slot, kFPed) = r.stand_ped;
    field(r.slot, kFCar) = r.stand_car;
  }
  r.swapped = false;
  r.dropped = true;
  unhide(r);
  log::info("Races: {} back to the script's AI in slot {} ({})", r.name, r.slot, why);
}

// ---- fingerprint

bool fingerprint(std::string& why) {
  constexpr int kWords = 1300;
  if (!readable_range(g_st, kWords * sizeof(std::int32_t))) return why = "statics not readable", false;
  if (st(kPlayerSlotWord) != kPlayerSlot) return why = "player slot word", false;
  if (st(kArrayCount) != kSlots) return why = "racer array count word", false;
  if (st(kRaceRacers) != kSlots) return why = "not a 4-racer race", false;
  const int type = st(kRaceType);
  if (type < 0 || type > 2) return why = std::format("race type {} (boats/helis/mixed stay vanilla)", type), false;
  g_checks = st(kRaceCheckpoints), g_laps = st(kRaceLaps);
  if (g_checks < 2 || g_checks > 60 || g_laps < 1 || g_laps > 20) return why = "checkpoint/lap counts", false;
  const n::Ped me = host_player();
  if (!me) return why = "no player", false;
  // The element size isn't in the decompile; find it from the player slot (its ped and model are set at race setup).
  int found = 0;
  for (int base0 : {1130, 1129})
    for (int stride = 19; stride <= 24; ++stride) {
      const int b = base0 + stride * kPlayerSlot;
      if (b + kFStatus >= kWords) continue;
      if (st(b + kFPed) == me && st(b + kFModel) == kPlayerCarModel) {
        g_base0 = base0, g_stride = stride;
        ++found;
      }
    }
  if (found != 1) return why = std::format("racer element size not unique ({} matches)", found), false;
  return true;
}

bool slot_ready(int slot) {
  const n::Ped p = field(slot, kFPed);
  const n::Vehicle c = field(slot, kFCar);
  return p && c && n::DoesCharExist(p) && n::DoesVehicleExist(c) && n::IsCharInCar(p, c);
}

// ---- standings (the script's own order: lap, then checkpoint, then distance to it)

std::vector<Rank> standings() {
  std::vector<Rank> v;
  for (int s = 0; s < kSlots; ++s)
    v.push_back({s, field(s, kFStatus) == 5, field(s, kFLap), field(s, kFCheck), fld_float(s, kFDist)});
  std::stable_sort(v.begin(), v.end(), [](const Rank& a, const Rank& b) {
    if (a.finished != b.finished) return a.finished;
    if (a.lap != b.lap) return a.lap > b.lap;
    if (a.check != b.check) return a.check > b.check;
    return a.dist < b.dist;
  });
  return v;
}

struct Entry {
  std::uint8_t id;
  std::string name;
};

Entry entry_for(int slot, const Frame& f) {
  if (slot == kPlayerSlot) {
    for (const Player& p : f.players)
      if (p.id == f.my_id) return {p.id, p.name.empty() ? std::string("You") : p.name};
    return {f.my_id, "Host"};
  }
  for (const Racer& r : g_racers)
    if (r.slot == slot && r.swapped) return {r.id, r.name};
  return {kNobody, "AI"};
}

void send_results(const Frame& f, bool finished) {
  const std::vector<Rank>& order = g_last;
  std::vector<Entry> list;
  for (const Rank& r : order) list.push_back(entry_for(r.slot, f));
  std::string text = finished ? "Race finished: " : "Race over: ";
  for (std::size_t i = 0; i < list.size(); ++i) text += std::format("{}{}. {}", i ? ", " : "", i + 1, list[i].name);
  phone::notify(text);
  log::info("Races: {}", text);
  for (const Racer& r : g_racers) {
    if (!r.swapped) continue;
    int pos = 0;
    for (std::size_t i = 0; i < list.size(); ++i)
      if (list[i].id == r.id) pos = static_cast<int>(i) + 1;
    auto w = msg(2, r.id);
    w.put(static_cast<std::uint8_t>(pos)).put(static_cast<std::uint8_t>(list.size()))
        .put(static_cast<std::uint8_t>(finished ? 1 : 0)).put(static_cast<std::uint8_t>(list.size()));
    for (const Entry& e : list) w.put(e.id).put(net::make_name(e.name));
    queue(std::move(w));
  }
}

void end_race(const Frame& f, bool statics_gone) {
  if (g_mode == Mode::Racing && statics_gone) {
    bool finished = false;
    if (g_last_valid && !g_last.empty()) {
      const Rank& lead = g_last.front();
      finished = lead.finished || (lead.lap == g_laps && lead.check >= g_checks - 1 && lead.dist < 40.0f);
    }
    if (finished) send_results(f, true);
    else
      for (const Racer& r : g_racers)
        if (r.swapped) queue(msg(3, r.id));
  }
  // Leftover stand-ins (the script's cleanup was aimed at them; anything it missed goes now).
  for (Racer& r : g_racers) {
    if (r.stand_ped && n::DoesCharExist(r.stand_ped)) n::DeleteChar(r.stand_ped);
    if (r.stand_car && n::DoesVehicleExist(r.stand_car)) n::DeleteCar(r.stand_car);
  }
  g_racers.clear();
  g_candidates.clear();
  g_last.clear();
  g_last_valid = false;
  g_go_sent = false;
  g_st = nullptr;
  g_mode = Mode::Idle;
}

void take_over(const Frame& f) {
  g_racers.clear();
  for (std::uint8_t id : g_candidates) {
    if (static_cast<int>(g_racers.size()) >= kMaxPartners) break;
    const auto it = std::find_if(f.players.begin(), f.players.end(), [&](const Player& p) { return p.id == id; });
    if (it == f.players.end() || it->id == f.my_id || !it->ped || !n::DoesCharExist(it->ped)) continue;
    const int slot = static_cast<int>(g_racers.size());
    Racer r;
    r.id = id, r.name = it->name.empty() ? std::format("Player {}", id) : it->name, r.slot = slot;
    r.stand_ped = field(slot, kFPed), r.stand_car = field(slot, kFCar);
    if (!r.stand_ped || !r.stand_car || !n::DoesCharExist(r.stand_ped) || !n::DoesVehicleExist(r.stand_car)) {
      log::warn("Races: slot {} has no AI car/driver; vanilla for that slot", slot);
      continue;
    }
    r.model = field(slot, kFModel);
    r.grid = {fld_float(slot, kFGrid), fld_float(slot, kFGrid + 1), fld_float(slot, kFGrid + 2)};
    r.heading = fld_float(slot, kFHeading);
    g_racers.push_back(r);
  }
  if (g_racers.empty()) {
    log::info("Races: nobody else at the start; vanilla race");
    g_mode = Mode::Vanilla;
    return;
  }
  for (Racer& r : g_racers) {
    hide(r);
    auto w = msg(0, r.id);
    w.put(static_cast<std::uint8_t>(r.slot)).put(static_cast<std::uint32_t>(r.model)).put(r.grid.x).put(r.grid.y)
        .put(r.grid.z).put(r.heading);
    queue(std::move(w));
    log::info("Races: {} takes grid slot {} (model 0x{:08X}, {:.0f} {:.0f} {:.0f})", r.name, r.slot,
              static_cast<std::uint32_t>(r.model), r.grid.x, r.grid.y, r.grid.z);
  }
  g_go_sent = false;
  g_run_at = 0;
  g_last_valid = false;
  g_mode = Mode::Racing;
}

void race_tick(const Frame& f) {
  const DWORD now = GetTickCount();
  if (st(kPlayerSlotWord) != kPlayerSlot || st(kArrayCount) != kSlots) {
    log::warn("Races: unexpected statics mid-race; leaving it to the script");
    for (Racer& r : g_racers) release_slot(r, "layout changed");
    g_mode = Mode::Vanilla;
    return;
  }
  const int stage = st(kStage);
  if (stage == 1 && !g_go_sent) {
    g_go_sent = true, g_run_at = now;
    for (const Racer& r : g_racers) queue(msg(1, r.id));
    log::info("Races: go");
  }
  for (Racer& r : g_racers) {
    if (r.dropped) continue;
    const auto it = std::find_if(f.players.begin(), f.players.end(), [&](const Player& p) { return p.id == r.id; });
    const n::Ped puppet = it != f.players.end() ? it->ped : 0;
    if (!r.swapped) {
      const n::Vehicle car = coop::puppet_vehicle(r.id);
      if (puppet && n::DoesCharExist(puppet) && car && n::DoesVehicleExist(car) && n::IsCharInCar(puppet, car) &&
          static_cast<std::int32_t>(n::GetCarModel(car)) == r.model) {
        r.puppet = puppet, r.car = car, r.swapped = true;
        field(r.slot, kFPed) = puppet;
        field(r.slot, kFCar) = car;
        log::info("Races: {} is racing in slot {} (ped {}, car {})", r.name, r.slot, puppet, car);
      } else if (g_go_sent && now - g_run_at > kSwapGraceMs) {
        release_slot(r, "not in their car when the race started");
      } else if (it == f.players.end()) {
        release_slot(r, "left the session");
      }
      continue;
    }
    if (!puppet || !n::DoesCharExist(puppet) || !n::DoesVehicleExist(r.car)) {
      release_slot(r, "partner or their car gone");
      continue;
    }
    r.puppet = puppet; // the puppet handle can change if it was respawned; the word follows
    if (field(r.slot, kFPed) != puppet) field(r.slot, kFPed) = puppet;
  }
  if (stage >= 1) {
    g_last = standings();
    g_last_valid = true;
  }
}

} // namespace

void register_hooks() {
  for (const Redirect& r : kRedirects)
    native_hooks::before(r.hash, r.name, [&r](const Call& c) { redirect(c, r); });
}

void host_tick(const Frame& f) {
  g_host_tick_at = GetTickCount();
  g_my_id = f.my_id;
  std::int32_t* now_st = activity_seats::statics("racesp");
  if (episode::current() != episode::Id::TBoGT) now_st = nullptr;
  if (!now_st) {
    if (g_mode == Mode::Racing) end_race(f, true);
    g_mode = Mode::Idle;
    g_st = nullptr;
    return;
  }
  if (now_st != g_st) { // a new race script
    if (g_mode == Mode::Racing) end_race(f, true);
    g_st = now_st;
    g_mode = Mode::Waiting;
    g_candidates.clear();
    g_hit_logged = false;
  }
  if (g_mode == Mode::Vanilla) return;
  if (g_mode == Mode::Waiting) {
    for (const Player* p : partners_near(f, kCandidateRange))
      if (std::find(g_candidates.begin(), g_candidates.end(), p->id) == g_candidates.end()) g_candidates.push_back(p->id);
    if (!readable_range(g_st, 1300 * sizeof(std::int32_t))) return;
    // Only a race seen before it starts: the grid is populated and the countdown is on.
    if (st(kStage) != 0 || st(kSubStage) != 5) {
      if (st(kStage) != 0) {
        log::info("Races: the race was already running; vanilla");
        g_mode = Mode::Vanilla;
      }
      return;
    }
    std::string why;
    if (!fingerprint(why)) {
      log::warn("Races: racesp statics don't match the expected layout ({}); vanilla race", why);
      g_mode = Mode::Vanilla;
      return;
    }
    take_over(f);
    return;
  }
  race_tick(f);
}

// Host: send whatever was queued.
void host_send(net::Session& net) {
  for (auto& d : g_out) net.send_game(std::move(d), true);
  g_out.clear();
}

// ---- partner

void client_tick(std::uint8_t my_id) {
  g_client_id = my_id;
  if (GetTickCount() - g_host_tick_at < 1000) return; // we are the host
  const DWORD now = GetTickCount();
  if (g_holding && now - g_hold_since > kHoldMaxMs) {
    if (g_my_car && n::DoesVehicleExist(g_my_car)) n::FreezeCarPosition(g_my_car, false);
    g_holding = false;
  }
  if (!g_have_grid) return;
  if (now - g_grid.since > 20000) { // the model never loaded
    g_have_grid = false;
    return;
  }
  const n::Ped me = host_player();
  if (!me) return;
  if (!g_grid.scene_requested) {
    n::invoke(kRequestCollisionAt, g_grid.at.x, g_grid.at.y, g_grid.at.z);
    n::invoke(kLoadScene, g_grid.at.x, g_grid.at.y, g_grid.at.z);
    g_grid.scene_requested = true;
  }
  n::RequestModel(g_grid.model);
  if (!n::HasModelLoaded(g_grid.model)) return;
  if (n::IsCharInAnyCar(me)) n::WarpCharFromCarToCoord(me, n::GetCharCoordinates(me));
  const n::Vehicle car = n::CreateCar(g_grid.model, g_grid.at);
  n::MarkModelAsNoLongerNeeded(g_grid.model);
  g_have_grid = false;
  if (!car) return;
  n::invoke(kSetCarHeading, car, g_grid.heading);
  n::invoke(kSetCarOnGroundProperly, car);
  n::WarpCharIntoCar(me, car);
  n::SetCarEngineOn(car, true, true);
  n::FreezeCarPosition(car, true); // held for the countdown; the host's "go" lets it go
  g_my_car = car, g_holding = true, g_hold_since = now;
  log::info("Races: on the grid, slot {}", g_grid.slot);
}

void on_message(const net::GameMessage& m) {
  if (m.from != net::kHostId) return;
  net::Reader r(m.data);
  std::uint8_t type, sender, kind, target;
  if (!r.get(type) || !r.get(sender) || !r.get(kind) || !r.get(target) || target != g_client_id) return;
  if (GetTickCount() - g_host_tick_at < 1000) return;
  switch (kind) {
    case 0: {
      Grid g;
      if (!r.get(g.slot) || !r.get(g.model) || !r.get(g.at.x) || !r.get(g.at.y) || !r.get(g.at.z) || !r.get(g.heading))
        return;
      g.since = GetTickCount();
      g_grid = g, g_have_grid = true;
      break;
    }
    case 1:
    case 3:
      g_have_grid = false;
      if (g_holding) {
        if (g_my_car && n::DoesVehicleExist(g_my_car)) n::FreezeCarPosition(g_my_car, false);
        g_holding = false;
      }
      break;
    case 2: {
      std::uint8_t pos, total, finished, count;
      if (!r.get(pos) || !r.get(total) || !r.get(finished) || !r.get(count) || count > 4) return;
      std::string text = std::format("Race: you came {} of {}. ", ordinal(pos), total);
      for (int i = 0; i < count; ++i) {
        std::uint8_t id;
        net::Name name{};
        if (!r.get(id) || !r.get(name)) return;
        text += std::format("{}{}. {}", i ? ", " : "", i + 1, net::to_string(name));
      }
      phone::notify(text);
      g_have_grid = false;
      if (g_holding) {
        if (g_my_car && n::DoesVehicleExist(g_my_car)) n::FreezeCarPosition(g_my_car, false);
        g_holding = false;
      }
      break;
    }
    default:
      break;
  }
}

} // namespace ivc::activities::races
