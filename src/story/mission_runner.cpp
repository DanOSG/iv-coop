#include "story/mission_runner.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/focus.h"
#include "core/log.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "game/script.h"
#include "story/coverage.h"
#include "story/mission_audit.h"
#include "sync/remote_players.h"

namespace ivc::story::runner {
namespace {
namespace n = natives;
using native_hooks::Call;

constexpr std::uint32_t kStartNewScript = 0x4E2260B9, kAddBlipForContact = 0x7C671162,
                        kLoadScene = 0x39F62BFB, kIsButtonPressed = 0x7FF21081, kIsButtonJustPressed = 0x016C37CD,
                        kIsControlPressed = 0x0E635761, kIsKeyboardNavUp = 0x14AB75AE;
// "Up" answers an incoming call; the phone reads it as pad button 8, control (2, 21) or keyboard nav-up.
constexpr std::int32_t kButtonDpadUp = 8, kControlPadUp = 21;
constexpr std::uint32_t kContactEveryMs = 20000; // time at each contact point for its mission to trigger
constexpr std::uint32_t kSettleMs = 10000;       // after a load: let the story flow set the world up
constexpr std::uint32_t kLoadTimeoutMs = 120000;
constexpr std::uint32_t kNoMissionMs = 300000;   // no mission started by then: give up on this save
constexpr std::uint32_t kQuitDelayMs = 3000;     // let the log and files flush

// Loading a save in place, the way the pause menu's Load Game does (GTAIV.exe 1.2.0.59, static
// analysis outside the protected region): the frontend action handler (0x5A3980, run every
// frame from the main loop) acts on a pending action while the frontend mode is 1. Action 0x3F
// waits until loading is safe, 0x40 follows, and 0x41 loads the selected slot (SGTA4<slot>)
// through the save system (0x8C7670) and rebuilds the world from it.
// Addresses are static VAs (image base 0x400000); the exe is relocated at run time.
namespace engine {
template <class T>
T at_static(std::uintptr_t va) {
  return reinterpret_cast<T>(va - 0x400000 + reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}
using SetInt = std::uint32_t(__cdecl*)(std::int32_t);
void set_load_slot(std::int32_t slot) { at_static<SetInt>(0x8BECB0)(slot); }     // [0x1030BA0] = slot
void set_frontend_mode(std::int32_t mode) { at_static<SetInt>(0x8BECC0)(mode); } // [0x1030B7C] = mode
void set_action(std::int32_t a) { at_static<SetInt>(0x8BE0E0)(a); }              // [0x1160C24] = action
std::int32_t action() { return *at_static<const volatile std::int32_t*>(0x1160C24); }
constexpr std::int32_t kLoadPrepare = 0x3F, kLoadSlot = 0x41;
} // namespace engine

struct Item {
  std::string script;
  int slot = -1;
  std::optional<n::Vector3> at; // its contact point (from main's tables), visited first
};

enum class Phase { Off, Booting, Loading, Waiting, Running, Done } g_phase = Phase::Off;
std::filesystem::path g_dir;
std::vector<Item> g_queue;
std::size_t g_index = 0;
unsigned g_seconds = 900;
std::string g_recording;
std::uint32_t g_phase_ms = 0, g_contact_ms = 0, g_loaded_ms = 0;
bool g_load_seen = false; // the load action ran (so what follows is the new world)
unsigned g_target_visits = 0;
// Contact blips the story flow put up: early and mid-game missions start when the player walks
// into one (the mission's own script doesn't exist until then).
struct Contact {
  n::Blip blip;
  n::Vector3 at;
};
std::vector<Contact> g_contacts;
std::size_t g_next_contact = 0;

const Item& item() { return g_queue[g_index]; }

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

void result(const std::string& mission, const std::string& outcome) {
  std::ofstream(g_dir / "runner_result.txt", std::ios::app)
      << item().script << '\t' << mission << '\t' << outcome << '\t' << g_recording << '\n';
}

void teleport(const n::Vector3& at) {
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (n::IsCharInAnyCar(me)) n::WarpCharFromCarToCoord(me, at);
  n::invoke(kLoadScene, at.x, at.y, at.z);
  n::SetCharCoordinatesNoOffset(me, {at.x, at.y, at.z + 1.0f});
}

// The phone is ringing or a call is on: teleporting now cuts the call off and the contact rings again.
bool phone_busy() {
  for (const auto& t : script::threads()) {
    const std::string name = lower(t.name);
    if (name == "spcellphonemain" || name == "spcellphonecalling") return true;
  }
  return false;
}

void visit_next_contact() {
  if (item().at && g_target_visits < 3) {
    ++g_target_visits;
    teleport(*item().at);
    log::info("Runner: walking into {}'s contact point", item().script);
    return;
  }
  std::erase_if(g_contacts, [](const Contact& c) { return !n::DoesBlipExist(c.blip); });
  if (g_contacts.empty() || g_next_contact >= 6) return; // then just wait: it may start with a phone call
  const Contact& c = g_contacts[g_next_contact++ % g_contacts.size()];
  teleport(c.at);
  log::info("Runner: walking into contact blip at {:.0f} {:.0f} {:.0f}", c.at.x, c.at.y, c.at.z);
}

// Behind every other window, without taking focus (the game keeps running: focus::keep_running).
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

void request_load() {
  const Item& it = item();
  g_contacts.clear(), g_next_contact = 0, g_target_visits = 0, g_load_seen = false, g_loaded_ms = 0, g_recording.clear();
  engine::set_load_slot(it.slot);
  engine::set_frontend_mode(1);
  engine::set_action(engine::kLoadPrepare);
  g_phase = Phase::Loading, g_phase_ms = GetTickCount();
  log::warn("Runner: [{}/{}] loading slot {} for {}", g_index + 1, g_queue.size(), it.slot, it.script);
}

// This save is finished with (played, or nothing to play): on to the next, or close the game.
void next(const std::string& mission, const std::string& outcome) {
  coop::stop_recording();
  coverage::flush();
  result(mission, outcome);
  log::warn("Runner: {} -> {}", mission, outcome);
  if (++g_index < g_queue.size()) {
    request_load();
    return;
  }
  log::warn("Runner: queue done; closing the game");
  g_phase = Phase::Done, g_phase_ms = GetTickCount();
}

// The first mission script the story flow starts in the loaded world is the one the save is
// waiting at (normally the target). Taken over straight away, inside the START_NEW_SCRIPT hook,
// so the audit sees the blip the mission makes next.
void begin(const std::string& mission) {
  if (mission != item().script) log::warn("Runner: main started {}, not {}; running it instead", mission, item().script);
  coop::host();
  const auto rec = g_dir / std::format("runner-{}.ivcrec", mission);
  g_recording = coop::start_recording(rec.string()) ? rec.filename().string() : "";
  g_phase = Phase::Running;
  audit::watch(mission, g_seconds, [mission](const std::string& outcome) { next(mission, outcome); });
}

} // namespace

void configure(const std::filesystem::path& plugin_dir) {
  g_dir = plugin_dir / "iv-coop";
  const auto file = g_dir / "runner.txt";
  if (!std::filesystem::exists(file)) return;
  std::ifstream in(file);
  // seconds=<per mission>; then one "run=<script>,<slot>[,x,y,z]" per mission, in order.
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == "seconds") {
      g_seconds = static_cast<unsigned>(std::stoul(value));
    } else if (key == "run") {
      Item it;
      char name[64] = {};
      n::Vector3 v{};
      const int got = std::sscanf(value.c_str(), "%63[^,],%d,%f,%f,%f", name, &it.slot, &v.x, &v.y, &v.z);
      if (got < 2) continue;
      it.script = lower(name);
      if (got == 5) it.at = v;
      g_queue.push_back(it);
    }
  }
  in.close();
  std::error_code ec;
  std::filesystem::rename(file, g_dir / "runner.active", ec);
  if (g_queue.empty()) return;
  g_phase = Phase::Booting;
  log::warn("Runner: {} missions queued (up to {} s each)", g_queue.size(), g_seconds);
}

namespace {

// Missions that start "now" (main's start type 1, e.g. after a contact phones) begin with an incoming call
// someone has to answer: while waiting for the mission and the phone is ringing (the calling script is up but
// the phone's UI script isn't: answering opens it), the phone scripts' read of d-pad up gets one short press.
// Pressing again during the call hangs up or skips lines, and the flow re-rings forever.
bool call_ringing() {
  bool ui = false, call = false;
  for (const auto& t : script::threads()) {
    const std::string name = lower(t.name);
    ui |= name == "spcellphonemain", call |= name == "spcellphonecalling";
  }
  return call && !ui;
}

void answer_calls(const Call& c) {
  if (g_phase != Phase::Waiting || !lower(c.script).starts_with("spcellphone")) return;
  const bool up = c.hash == kIsKeyboardNavUp ||
                  (c.hash == kIsControlPressed ? c.arg<std::int32_t>(0) == 2 && c.arg<std::int32_t>(1) == kControlPadUp
                                               : c.arg<std::int32_t>(1) == kButtonDpadUp);
  if (!up) return;
  static std::uint32_t pressed_at = 0;
  const std::uint32_t now = GetTickCount();
  if (now - pressed_at > 3000) {
    if (!call_ringing()) return;
    pressed_at = now;
    log::info("Runner: answering the phone");
  }
  if (now - pressed_at < 300) *reinterpret_cast<std::int32_t*>(c.ctx->result) = 1;
}

} // namespace

void register_hooks() {
  if (g_phase == Phase::Off) return;
  native_hooks::on(kIsButtonPressed, "IS_BUTTON_PRESSED", answer_calls);
  native_hooks::on(kIsButtonJustPressed, "IS_BUTTON_JUST_PRESSED", answer_calls);
  native_hooks::on(kIsControlPressed, "IS_CONTROL_PRESSED", answer_calls);
  native_hooks::on(kIsKeyboardNavUp, "IS_GAME_KEYBOARD_NAV_UP_PRESSED", answer_calls);
  native_hooks::on(kStartNewScript, "START_NEW_SCRIPT", [](const Call& c) {
    if (g_phase != Phase::Waiting && !(g_phase == Phase::Loading && g_load_seen)) return;
    const std::string name = lower(c.str(0));
    if (lower(c.script) == "main" || name == item().script) begin(name);
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
    case Phase::Booting: // whatever save the game loaded at start: settle, then load ours
      if (!n::GetPlayerChar(n::GetPlayerId())) return;
      if (!g_phase_ms) g_phase_ms = now;
      if (now - g_phase_ms > kSettleMs) coop::host(), request_load();
      break;
    case Phase::Loading:
      if (engine::action() == engine::kLoadSlot) g_load_seen = true;
      if (g_load_seen && engine::action() != engine::kLoadSlot && n::GetPlayerChar(n::GetPlayerId())) {
        if (!g_loaded_ms) g_loaded_ms = now; // settle from here
        if (now - g_loaded_ms > kSettleMs) {
          g_phase = Phase::Waiting, g_phase_ms = now, g_contact_ms = 0;
          log::warn("Runner: slot {} loaded; waiting for {}", item().slot, item().script);
        }
      } else if (now - g_phase_ms > kLoadTimeoutMs) {
        next("-", "the save didn't load");
      }
      break;
    case Phase::Waiting:
      if (now - g_phase_ms > kNoMissionMs) next("-", "no mission started");
      else if (phone_busy()) g_contact_ms = now; // let the call finish first
      else if (now - g_contact_ms > kContactEveryMs) g_contact_ms = now, visit_next_contact();
      break;
    case Phase::Running:
      break;
    case Phase::Done:
      if (now - g_phase_ms > kQuitDelayMs) TerminateProcess(GetCurrentProcess(), 0);
      break;
    case Phase::Off:
      break;
  }
}

} // namespace ivc::story::runner
