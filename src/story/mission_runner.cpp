#include "story/mission_runner.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "core/focus.h"
#include "core/log.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "story/coverage.h"
#include "story/mission_audit.h"
#include "sync/remote_players.h"

namespace ivc::story::runner {
namespace {
namespace n = natives;
using native_hooks::Call;

constexpr std::uint32_t kStartNewScript = 0x4E2260B9, kAddBlipForContact = 0x7C671162,
                        kLoadScene = 0x39F62BFB;
constexpr std::uint32_t kContactEveryMs = 20000; // time at each contact blip for its mission to trigger
constexpr std::uint32_t kSettleMs = 10000;     // after the first frame with a player: the save has loaded
constexpr std::uint32_t kNoMissionMs = 300000; // no mission started by then: give up
constexpr std::uint32_t kQuitDelayMs = 3000;     // let the log and files flush

enum class Phase { Off, Loading, Waiting, Running, Quitting } g_phase = Phase::Off;
std::filesystem::path g_dir;
std::string g_target;
unsigned g_seconds = 900;
std::string g_recording;
std::uint32_t g_phase_ms = 0, g_contact_ms = 0;
// Contact blips the story flow put up: early and mid-game missions start when the player walks
// into one (the mission's own script doesn't exist until then), so the runner visits them in turn.
struct Contact {
  n::Blip blip;
  n::Vector3 at;
};
std::vector<Contact> g_contacts;
std::size_t g_next_contact = 0;
// Where the target's contact blip goes (from main's mission and location tables, via runner.txt
// "at=x,y,z"); visited first. Missions with no location are started by main or a phone call.
std::optional<n::Vector3> g_target_at;
unsigned g_target_visits = 0;

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

void result(const std::string& mission, const std::string& outcome) {
  std::ofstream(g_dir / "runner_result.txt", std::ios::app)
      << g_target << '\t' << mission << '\t' << outcome << '\t' << g_recording << '\n';
}

// Behind every other window, without taking focus (the game keeps running: focus::keep_running).
void teleport(const n::Vector3& at) {
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (n::IsCharInAnyCar(me)) n::WarpCharFromCarToCoord(me, at);
  n::invoke(kLoadScene, at.x, at.y, at.z);
  n::SetCharCoordinatesNoOffset(me, {at.x, at.y, at.z + 1.0f});
}

void visit_next_contact() {
  if (g_target_at && g_target_visits < 3) {
    ++g_target_visits;
    teleport(*g_target_at);
    log::info("Runner: walking into {}'s contact point", g_target);
    return;
  }
  std::erase_if(g_contacts, [](const Contact& c) { return !n::DoesBlipExist(c.blip); });
  if (g_contacts.empty()) return;
  const Contact& c = g_contacts[g_next_contact++ % g_contacts.size()];
  teleport(c.at);
  log::info("Runner: walking into contact blip at {:.0f} {:.0f} {:.0f}", c.at.x, c.at.y, c.at.z);
}

void send_window_back() {
  struct Find {
    static BOOL CALLBACK cb(HWND w, LPARAM out) {
      DWORD pid = 0;
      GetWindowThreadProcessId(w, &pid);
      if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
      *reinterpret_cast<HWND*>(out) = w;
      return FALSE;
    }
  };
  HWND w = nullptr;
  EnumWindows(Find::cb, reinterpret_cast<LPARAM>(&w));
  if (w) SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void quit(const std::string& mission, const std::string& outcome) {
  coop::stop_recording();
  coverage::flush();
  result(mission, outcome);
  log::warn("Runner: {} -> {}; closing the game", mission, outcome);
  g_phase = Phase::Quitting, g_phase_ms = GetTickCount();
}

// The first mission script the story flow starts is the one the save is waiting at (normally the
// target). Taken over straight away, inside the START_NEW_SCRIPT hook, so the audit sees the blip
// the mission makes next.
void begin(const std::string& mission) {
  if (mission != g_target) log::warn("Runner: main started {}, not {}; running it instead", mission, g_target);
  coop::host();
  const auto rec = g_dir / std::format("runner-{}.ivcrec", mission);
  g_recording = coop::start_recording(rec.string()) ? rec.filename().string() : "";
  g_phase = Phase::Running;
  audit::watch(mission, g_seconds, [mission](const std::string& outcome) { quit(mission, outcome); });
}

} // namespace

void configure(const std::filesystem::path& plugin_dir) {
  g_dir = plugin_dir / "iv-coop";
  const auto file = g_dir / "runner.txt";
  if (!std::filesystem::exists(file)) return;
  std::ifstream in(file);
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == "mission") g_target = lower(value);
    else if (key == "seconds") g_seconds = static_cast<unsigned>(std::stoul(value));
    else if (key == "at") {
      n::Vector3 v{};
      if (std::sscanf(value.c_str(), "%f,%f,%f", &v.x, &v.y, &v.z) == 3) g_target_at = v;
    }
  }
  in.close();
  std::error_code ec;
  std::filesystem::rename(file, g_dir / "runner.active", ec);
  if (g_target.empty()) return;
  g_phase = Phase::Loading;
  log::warn("Runner: will run {} (up to {} s) once the save has loaded", g_target, g_seconds);
}

void register_hooks() {
  if (g_phase == Phase::Off) return;
  // The story flow starts mission scripts itself once the save has loaded.
  native_hooks::on(kStartNewScript, "START_NEW_SCRIPT", [](const Call& c) {
    if (g_phase != Phase::Loading && g_phase != Phase::Waiting) return;
    const std::string name = lower(c.str(0));
    if (lower(c.script) == "main" || name == g_target) begin(name);
  });
  native_hooks::on(kAddBlipForContact, "ADD_BLIP_FOR_CONTACT", [](const Call& c) {
    if (lower(c.script) == "main") g_contacts.push_back({c.out<n::Blip>(3), {c.arg<float>(0), c.arg<float>(1), c.arg<float>(2)}});
  });
}

void tick() {
  if (g_phase == Phase::Off) return;
  const std::uint32_t now = GetTickCount();
  static bool backgrounded = false;
  if (!backgrounded && n::GetPlayerChar(n::GetPlayerId())) {
    backgrounded = true;
    focus::keep_running(true);
    send_window_back();
  }
  switch (g_phase) {
    case Phase::Loading:
      if (!n::GetPlayerChar(n::GetPlayerId())) return;
      if (!g_phase_ms) g_phase_ms = now;
      if (now - g_phase_ms < kSettleMs) return;
      g_phase = Phase::Waiting, g_phase_ms = now;
      log::warn("Runner: save loaded; waiting for {}", g_target);
      break;
    case Phase::Waiting:
      if (now - g_phase_ms > kNoMissionMs) quit("-", "no mission started");
      else if (now - g_contact_ms > kContactEveryMs) g_contact_ms = now, visit_next_contact();
      break;
    case Phase::Running:
      break;
    case Phase::Quitting:
      if (now - g_phase_ms > kQuitDelayMs) TerminateProcess(GetCurrentProcess(), 0);
      break;
    case Phase::Off:
      break;
  }
}

} // namespace ivc::story::runner
