#include "story/mission_audit.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include "core/focus.h"
#include "core/input.h"
#include "core/log.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "game/script.h"

namespace ivc::story::audit {
namespace {
namespace n = natives;
using n::invoke;
using native_hooks::Call;

constexpr std::uint32_t kDoesScriptExist = 0x1D1B266B, kRequestScript = 0x6FFE0DFD, kHasScriptLoaded = 0x2A171915,
                        kStartNewScript = 0x4E2260B9, kMarkScriptNoLongerNeeded = 0x09E405DB,
                        kTerminateScripts = 0x72452672, kGetBlipCoords = 0x4C1E75DB, kClearWanted = 0x205622AC,
                        kFadeIn = 0x04D72200, kSetPlayerControl = 0x1A6203EA, kDeleteObject = 0x62FE6290,
                        kDoesObjectExist = 0x6DAB78CD, kClearCutscene = 0x79611458, kSetInvincible = 0x7E9E02E1,
                        kClearPrints = 0x1D8C324A, kClearHelp = 0x07244253, kHasCutsceneFinished = 0x4ECE1AD2,
                        kConversationOngoing = 0x3CA23254, kGetBlipColour = 0x59B425DA, kExplodeCar = 0x505518A2,
                        kSetCharHealth = 0x575E2880, kIsCarDead = 0x2AAB340A, kStopCutscene = 0x50FF1428,
                        kGetCutsceneTime = 0x7DF26C8C;
constexpr std::int32_t kHostileColour = 1; // IV blip colour red: enemies/targets (the default for peds)

constexpr unsigned kNudgeEvery = 5;   // s between teleports to the newest blip
constexpr unsigned kIdleEnd = 20;      // s without a new call: the mission is waiting on us
constexpr unsigned kWatchIdleEnd = 90; // same, for a mission the runner plays (driving parts are quiet)
constexpr unsigned kGapSeconds = 5;   // between missions
constexpr unsigned kLoadTimeout = 15; // s

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

std::filesystem::path audit_dir() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(GetModuleHandleW(L"iv-coop.asi"), buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path() / "iv-coop" / "audit";
}

// What one mission did (its own script plus the scripts it started).
struct Report {
  std::string name;
  std::string outcome;
  unsigned seconds = 0, nudges = 0;
  std::map<std::string, unsigned> counts; // native -> calls
  std::vector<std::string> notes;         // notable calls, in order
  std::set<std::string> family;           // lowercase script names
  std::vector<std::pair<char, std::int32_t>> entities; // 'p'ed / 'c'ar / 'o'bject handles
  struct Blip {
    std::int32_t blip;
    char kind; // 'x' coord/contact, 'p' ped, 'c' car, 'o' object
    std::int32_t entity;
  };
  std::vector<Blip> blips; // live blips, newest last
  bool cutscene = false;
  unsigned skipped = 0;
};

Settings g_cfg;
bool g_down = true, g_active = false;
std::function<void(const std::string&)> g_watch_done; // set: watching one mission for the runner
unsigned g_watch_seconds = 0;
std::vector<std::string> g_list;
std::size_t g_index = 0;
enum class Phase { Next, Loading, Running, Gap } g_phase = Phase::Next;
std::uint32_t g_phase_ms = 0, g_last_nudge_ms = 0;
std::uint32_t g_last_call_ms = 0; // last hooked call by the mission's scripts
std::uint32_t g_cutscene_ms = 0;  // a cutscene started (0: none to skip)
unsigned g_skipped = 0;
Report g_rep;
n::Vector3 g_home{};

void note(const Call& c, std::string detail) {
  if (g_rep.notes.size() < 400) g_rep.notes.push_back(std::format("{:<14} {}({})", c.script, c.name, detail));
}

// Every hooked native, after it ran.
void observe(const Call& c) {
  if (!g_active || g_phase != Phase::Running || !g_rep.family.contains(lower(c.script))) return;
  if (std::string_view(c.name).starts_with("IS_")) return; // per-frame queries (control checks): not progress
  ++g_rep.counts[c.name];
  g_last_call_ms = GetTickCount();
  const std::string nm = c.name;
  if (nm == "START_NEW_SCRIPT") {
    g_rep.family.insert(lower(c.str(0)));
    note(c, c.str(0));
  } else if (nm == "CREATE_CHAR") {
    g_rep.entities.push_back({'p', c.out<std::int32_t>(5)});
    note(c, std::format("model 0x{:08X}", c.arg<std::uint32_t>(1)));
  } else if (nm == "CREATE_CHAR_INSIDE_CAR" || nm == "CREATE_CHAR_AS_PASSENGER") {
    g_rep.entities.push_back({'p', c.out<std::int32_t>(nm == "CREATE_CHAR_INSIDE_CAR" ? 3 : 4)});
    note(c, std::format("model 0x{:08X}", c.arg<std::uint32_t>(2)));
  } else if (nm == "CREATE_CAR") {
    g_rep.entities.push_back({'c', c.out<std::int32_t>(4)});
    note(c, std::format("model 0x{:08X}", c.arg<std::uint32_t>(0)));
  } else if (nm == "CREATE_OBJECT") {
    g_rep.entities.push_back({'o', c.out<std::int32_t>(4)});
    note(c, std::format("model 0x{:08X}", c.arg<std::uint32_t>(0)));
  } else if (nm == "ADD_BLIP_FOR_COORD" || nm == "ADD_BLIP_FOR_CONTACT") {
    g_rep.blips.push_back({c.out<std::int32_t>(3), 'x', 0});
    note(c, std::format("{:.0f} {:.0f} {:.0f}", c.arg<float>(0), c.arg<float>(1), c.arg<float>(2)));
  } else if (nm == "ADD_BLIP_FOR_CHAR" || nm == "ADD_BLIP_FOR_CAR" || nm == "ADD_BLIP_FOR_OBJECT") {
    g_rep.blips.push_back({c.out<std::int32_t>(1), nm == "ADD_BLIP_FOR_CHAR" ? 'p' : nm == "ADD_BLIP_FOR_CAR" ? 'c' : 'o',
                           c.arg<std::int32_t>(0)});
    note(c, "");
  } else if (nm == "REMOVE_BLIP") {
    const auto gone = c.arg<std::int32_t>(0);
    std::erase_if(g_rep.blips, [gone](const Report::Blip& b) { return b.blip == gone; });
  } else if (nm == "START_CUTSCENE_NOW") {
    g_rep.cutscene = true;
    g_cutscene_ms = GetTickCount();
    note(c, c.str(0));
  } else if (nm == "REQUEST_ADDITIONAL_TEXT" || nm == "PRINT_HELP" || nm == "PRINT_NOW" || nm == "PRINT") {
    note(c, c.str(0));
  } else if (nm == "ADD_LINE_TO_CONVERSATION") {
    note(c, std::format("{}, {}", c.str(1), c.str(2)));
  } else if (nm == "ADD_LINE_TO_MOBILE_PHONE_CALL") {
    note(c, std::format("{}, {}", c.str(1), c.str(2)));
  } else if (nm == "START_MOBILE_PHONE_CALL") {
    note(c, std::format("{} -> {}", c.str(1), c.str(3)));
  } else if (nm == "ADD_NEW_CONVERSATION_SPEAKER") {
    note(c, std::format("{}, {}", c.arg<int>(0), c.str(2)));
  } else if (nm == "ALTER_WANTED_LEVEL" || nm == "SET_MAX_WANTED_LEVEL" || nm == "ADD_SCORE") {
    note(c, std::format("{}", c.arg<int>(nm == "SET_MAX_WANTED_LEVEL" ? 0 : 1)));
  } else if (nm == "REQUEST_IPL" || nm == "REMOVE_IPL" || nm == "SWITCH_ROADS_OFF" ||
             nm == "SET_STATE_OF_CLOSEST_DOOR_OF_TYPE" || nm == "SWAP_NEAREST_BUILDING_MODEL") {
    note(c, nm.find("IPL") != std::string::npos ? c.str(0) : "");
  }
}

std::vector<std::string> discover() {
  static const char* kPrefixes[] = {
      "roman", "michelle", "jacob", "brucie", "vlad", "faustin", "dwayne", "playboy", "elizabeta", "manny",
      "packie", "mcreary", "derrick", "francis", "gerry", "bernie", "ray", "jimmy", "pegorino", "phil", "bell",
      "dimitri", "kate", "brian", "ilyena", "mel", "eddie", "ivan", "marnie", "pathos", "cherise", "clarence",
      "gracie", "hossan", "jeff", "sara", "badman", "gerald", "mallorie", "little_jacob", "jon", "finale",
      "gambetti", "bulgarin", "mcr", "derrick_mcreary", "francis_mcreary", "ray_bulgarin", "lj", "pb", "fin"};
  std::vector<std::string> found;
  for (const char* p : kPrefixes)
    for (int i = 1; i <= 15; ++i)
      for (const char* sep : {"", "_"}) {
        const std::string name = std::format("{}{}{}", p, sep, i);
        if (invoke<bool>(kDoesScriptExist, name.c_str())) found.push_back(name);
      }
  return found;
}

void load_list() {
  std::set<std::string> done;
  {
    std::ifstream in(audit_dir() / "done.txt");
    for (std::string line; std::getline(in, line);)
      if (!line.empty()) done.insert(line);
  }
  std::vector<std::string> all = g_cfg.missions.empty() ? discover() : g_cfg.missions;
  {
    std::ofstream out(audit_dir() / "missions.txt");
    for (const auto& m : all) out << m << '\n';
  }
  g_list.clear();
  for (const auto& m : all)
    if (!done.contains(m)) g_list.push_back(m);
  log::warn("Audit: {} missions found, {} left to audit (list in {})", all.size(), g_list.size(),
            (audit_dir() / "missions.txt").string());
}

void write_report() {
  const Report& r = g_rep;
  std::ofstream out(audit_dir() / (r.name + ".md"));
  out << "# " << r.name << "\n\n";
  out << std::format("- Outcome: {}\n- Watched: {} s, {} nudges to blips, {} cutscenes skipped\n", r.outcome,
                     r.seconds, r.nudges, r.skipped);
  std::string fam;
  for (const auto& f : r.family) fam += (fam.empty() ? "" : ", ") + f;
  out << "- Scripts: " << fam << "\n\n## Calls\n\n| Native | Calls |\n|---|---|\n";
  std::vector<std::pair<std::string, unsigned>> sorted(r.counts.begin(), r.counts.end());
  std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
  for (const auto& [k, v] : sorted) out << "| " << k << " | " << v << " |\n";
  out << "\n## Notable calls, in order\n\n```\n";
  for (const auto& l : r.notes) out << l << '\n';
  out << "```\n";
  std::ofstream(audit_dir() / "done.txt", std::ios::app) << r.name << '\n';
}

n::Ped me() { return n::GetPlayerChar(n::GetPlayerId()); }

// In a car, the car goes too ("drive X to Y" objectives need it there).
void teleport(const n::Vector3& to) {
  const n::Ped p = me();
  if (n::IsCharInAnyCar(p)) {
    if (const n::Vehicle car = n::StoreCarCharIsInNoSave(p)) {
      n::SetCarCoordinatesNoOffset(car, to);
      return;
    }
    n::WarpCharFromCarToCoord(p, to);
  }
  n::SetCharCoordinatesNoOffset(p, to);
}

// Move the mission on to its next objective, judged by the blip: a hostile (red) ped is
// killed and a hostile car blown up ("kill/destroy X"); any other car is entered (driver,
// or a free passenger seat if someone drives it); otherwise the player goes to the blip.
// Some missions will fail this way (e.g. "follow, don't kill"); their fail path is data too.
bool nudge(const Report::Blip& b) {
  if (!n::DoesBlipExist(b.blip)) return false;
  const n::Ped p = me();
  std::int32_t colour = -1;
  invoke(kGetBlipColour, b.blip, &colour);
  if (b.kind == 'p' && colour == kHostileColour && n::DoesCharExist(b.entity) && !n::IsCharDead(b.entity)) {
    invoke(kSetCharHealth, b.entity, 0u);
    g_rep.notes.push_back(std::format("[audit] killed blipped hostile ped {} (blip colour {})", b.entity, colour));
    return true;
  }
  if (b.kind == 'c' && colour == kHostileColour && n::DoesVehicleExist(b.entity) && !invoke<bool>(kIsCarDead, b.entity)) {
    invoke(kExplodeCar, b.entity, true, false);
    g_rep.notes.push_back(std::format("[audit] blew up blipped hostile car {} (blip colour {})", b.entity, colour));
    return true;
  }
  g_rep.notes.push_back(std::format("[audit] nudge to {} blip (colour {})", b.kind, colour));
  if (b.kind == 'c' && n::DoesVehicleExist(b.entity)) {
    if (n::IsCharInCar(p, b.entity)) return false; // already there; the mission is busy
    const n::Ped driver = n::GetDriverOfCar(b.entity);
    if (!driver) {
      n::WarpCharIntoCar(p, b.entity);
      return true;
    }
    const std::uint32_t seats = n::GetMaximumNumberOfPassengers(b.entity);
    for (std::uint32_t seat = 0; seat < seats && seat < 8; ++seat)
      if (n::IsCarPassengerSeatFree(b.entity, seat)) {
        n::WarpCharIntoCarAsPassenger(p, b.entity, seat);
        return true;
      }
  }
  n::Vector3 at{};
  invoke(kGetBlipCoords, b.blip, &at);
  if (at.x == 0 && at.y == 0) return false;
  teleport({at.x + 2.0f, at.y + 2.0f, at.z + 1.0f});
  return true;
}

void cleanup() {
  for (const auto& s : g_rep.family) invoke(kTerminateScripts, s.c_str());
  for (auto [kind, h] : g_rep.entities) {
    if (!h) continue;
    if (kind == 'p' && n::DoesCharExist(h)) n::DeleteChar(h);
    else if (kind == 'c' && n::DoesVehicleExist(h)) n::DeleteCar(h);
    else if (kind == 'o' && invoke<bool>(kDoesObjectExist, h)) invoke(kDeleteObject, &h);
  }
  for (const auto& b : g_rep.blips)
    if (n::DoesBlipExist(b.blip)) n::RemoveBlip(b.blip);
  if (g_rep.cutscene && !invoke<bool>(kHasCutsceneFinished)) invoke(kStopCutscene);
  invoke(kClearPrints);
  invoke(kClearHelp);
  invoke(kClearWanted, n::GetPlayerId());
  invoke(kFadeIn, 0u);
  invoke(kSetPlayerControl, n::GetPlayerId(), true);
  teleport(g_home);
}

bool family_running() {
  for (const auto& t : script::threads())
    if (g_rep.family.contains(lower(t.name))) return true;
  return false;
}

void finish(const std::string& outcome, std::uint32_t now) {
  g_rep.outcome = outcome;
  g_rep.seconds = (now - g_phase_ms) / 1000;
  g_rep.skipped = g_skipped;
  g_skipped = 0;
  cleanup();
  write_report();
  log::warn("Audit: {} done ({}), {} natives, {} notes", g_rep.name, outcome, g_rep.counts.size(), g_rep.notes.size());
  if (g_watch_done) {
    invoke(kSetInvincible, n::GetPlayerId(), false);
    g_active = false;
    std::exchange(g_watch_done, nullptr)(outcome);
    return;
  }
  g_phase = Phase::Gap, g_phase_ms = now;
  ++g_index;
}

void stop(const char* why) {
  if (g_phase == Phase::Running || g_phase == Phase::Loading) cleanup();
  invoke(kSetInvincible, n::GetPlayerId(), false);
  focus::keep_running(false);
  g_active = false;
  log::warn("Audit: stopped ({}); {} of {} done this run", why, g_index, g_list.size());
}

} // namespace

void configure(const Settings& s) { g_cfg = s; }

void watch(const std::string& script, unsigned seconds, std::function<void(const std::string&)> done) {
  std::filesystem::create_directories(audit_dir());
  g_rep = Report{};
  g_rep.name = lower(script);
  g_rep.family = {g_rep.name};
  g_home = n::GetCharCoordinates(me());
  g_watch_done = std::move(done), g_watch_seconds = seconds;
  g_active = true, g_phase = Phase::Running, g_skipped = 0, g_cutscene_ms = 0;
  const std::uint32_t now = GetTickCount();
  g_phase_ms = g_last_nudge_ms = g_last_call_ms = now;
  invoke(kSetInvincible, n::GetPlayerId(), true);
  log::warn("Audit: watching {} for up to {} s", g_rep.name, seconds);
}

// A mission the runner plays waits on prompts ("Press ~INPUT_PICKUP~ to trigger the bomb"):
// answer the mission's own control checks with a short press every few seconds, per control.
void press_controls(const Call& c, int control_arg) {
  if (!g_watch_done || g_phase != Phase::Running || !g_rep.family.contains(lower(c.script))) return;
  static std::map<std::pair<std::uint32_t, std::int32_t>, std::uint32_t> pressed_at;
  const auto key = std::pair{c.hash, c.arg<std::int32_t>(control_arg)};
  const std::uint32_t now = GetTickCount();
  std::uint32_t& at = pressed_at[key];
  if (now - at > 4000) at = now;
  if (now - at < 300) *reinterpret_cast<std::int32_t*>(c.ctx->result) = 1;
}

void register_hooks() {
  native_hooks::observe(observe);
  // (pad, control) and (pad, button): the control/button is argument 1.
  native_hooks::on(0x0E635761, "IS_CONTROL_PRESSED", [](const Call& c) { press_controls(c, 1); });
  native_hooks::on(0x4CB729F1, "IS_CONTROL_JUST_PRESSED", [](const Call& c) { press_controls(c, 1); });
  native_hooks::on(0x7FF21081, "IS_BUTTON_PRESSED", [](const Call& c) { press_controls(c, 1); });
  native_hooks::on(0x016C37CD, "IS_BUTTON_JUST_PRESSED", [](const Call& c) { press_controls(c, 1); });
}

void tick() {
  if (input::pressed_ctrl_shift('A', g_down)) {
    if (g_active) {
      stop("Ctrl+Shift+A");
      return;
    }
    std::filesystem::create_directories(audit_dir());
    load_list();
    if (g_list.empty()) return;
    g_active = true, g_index = 0, g_phase = Phase::Next;
    g_home = n::GetCharCoordinates(me());
    focus::keep_running(true); // runs unattended in the background (windowed mode)
    invoke(kSetInvincible, n::GetPlayerId(), true); // a mission death would end the run
  }
  if (!g_active) return;

  const std::uint32_t now = GetTickCount();
  switch (g_phase) {
    case Phase::Next: {
      if (g_index >= g_list.size()) {
        stop("all missions audited");
        return;
      }
      g_rep = Report{};
      g_rep.name = g_list[g_index];
      g_rep.family = {lower(g_rep.name)};
      log::warn("Audit: [{}/{}] {}", g_index + 1, g_list.size(), g_rep.name);
      invoke(kRequestScript, g_rep.name.c_str());
      g_phase = Phase::Loading, g_phase_ms = now;
      break;
    }
    case Phase::Loading:
      if (invoke<bool>(kHasScriptLoaded, g_rep.name.c_str())) {
        const auto id = invoke<std::uint32_t>(kStartNewScript, g_rep.name.c_str(), g_cfg.stack);
        invoke(kMarkScriptNoLongerNeeded, g_rep.name.c_str());
        log::info("Audit: started {} (thread {})", g_rep.name, id);
        g_phase = Phase::Running, g_phase_ms = now, g_last_nudge_ms = now, g_last_call_ms = now;
      } else if (now - g_phase_ms > kLoadTimeout * 1000) {
        g_rep.outcome = "script did not load";
        write_report();
        ++g_index;
        g_phase = Phase::Gap, g_phase_ms = now;
      }
      break;
    case Phase::Running:
      // Skip cutscenes a couple of seconds in, once they are actually playing. STOP_CUTSCENE
      // ends playback the way a skip does; the mission then sees HAS_CUTSCENE_FINISHED and
      // frees it itself (CLEAR_NAMED_CUTSCENE on its own thread, as normal). Calling
      // CLEAR_CUTSCENE ourselves freed it under the render thread and crashed (roman5).
      if (g_cfg.skip_cutscenes && g_cutscene_ms && now - g_cutscene_ms > 2000 &&
          invoke<std::int32_t>(kGetCutsceneTime) > 500) {
        g_cutscene_ms = 0;
        if (!invoke<bool>(kHasCutsceneFinished)) {
          invoke(kStopCutscene);
          ++g_skipped;
          g_last_call_ms = now;
          log::info("Audit: stopped a cutscene in {}", g_rep.name);
        }
      }
      // A playing cutscene or conversation is the mission busy, not idle.
      if ((g_rep.cutscene && !invoke<bool>(kHasCutsceneFinished)) || invoke<bool>(kConversationOngoing))
        g_last_call_ms = now;
      if (now - g_phase_ms > 2000 && !family_running()) {
        finish("the mission's scripts ended by themselves", now);
      } else if (now - g_phase_ms > (g_watch_done ? g_watch_seconds : g_cfg.seconds) * 1000) {
        finish("timed out (stopped by the audit)", now);
      } else if (const unsigned idle = g_watch_done ? kWatchIdleEnd : kIdleEnd; now - g_last_call_ms > idle * 1000) {
        finish(std::format("idle for {} s (stopped by the audit)", idle), now);
      } else if (now - g_last_nudge_ms > kNudgeEvery * 1000 && !g_rep.blips.empty()) {
        g_last_nudge_ms = now;
        // Newest blip first; if it needs nothing (e.g. we're already in that car), an older one.
        for (auto it = g_rep.blips.rbegin(); it != g_rep.blips.rend(); ++it)
          if (nudge(*it)) {
            ++g_rep.nudges;
            break;
          }
      }
      break;
    case Phase::Gap:
      if (now - g_phase_ms > kGapSeconds * 1000) g_phase = Phase::Next;
      break;
  }
}

} // namespace ivc::story::audit
