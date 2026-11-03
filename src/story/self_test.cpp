#include "story/self_test.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/focus.h"
#include "core/log.h"
#include "game/episode.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "game/phone.h"
#include "game/script.h"
#include "sync/activity_seats.h"
#include "sync/episode_sync.h"
#include "sync/race_sync.h"
#include "sync/remote_players.h"

namespace ivc::story::self_test {
namespace {
namespace n = natives;
using native_hooks::Call;

constexpr std::uint32_t kIsControlPressed = 0x0E635761, kHasCharGotWeapon = 0x11F759DE, kRemoveWeapon = 0x2485231E;
constexpr std::uint32_t kBotWeapon = 7; // the bot host gives it through player_mirror
constexpr std::int32_t kControlPhoneUp = 21; // (2, 21): "up" on the phone scripts opens the phone
constexpr std::int32_t kControlSkip = 77;    // accept: pad A; scripted intros skip on it
constexpr std::int32_t kScreenMainMenu = 1011, kScreenClose = 1001, kIdMultiplayer = 2000, kIdHost = 2001;
constexpr std::uint32_t kSwitchTimeoutMs = 240000, kSettleMs = 20000, kPhoneTimeoutMs = 420000, kPhoneStepMs = 2000,
                        kJoinTimeoutMs = 45000, kHoldMs = 15000, kBotStartMs = 5000, kRefuseTimeoutMs = 25000,
                        kRestoreTimeoutMs = 20000, kQuitDelayMs = 3000;

enum class Step {
  Off, Boot, Switch, Settle, PhoneOpen, PhoneMenu, PhoneCoop, PhoneClose, Host, HostHold, JoinSame, JoinSameHold,
  Restore, JoinOther, Next, Done
};

std::filesystem::path g_dir;
std::vector<episode::Id> g_episodes;
std::size_t g_index = 0;
Step g_step = Step::Off;
std::uint32_t g_step_ms = 0, g_press_ms = 0, g_mark_ms = 0;
std::uint32_t g_protagonist = 0; // the player's model in this episode before any session
unsigned g_refusals_before = 0;
bool g_joined_seen = false;
// Activity seats: host side (the bot seated, its input arriving) and joiner side (seated by the bot host).
std::uint8_t g_seat_bot = 0xFF;
bool g_seat_input_seen = false, g_seated_seen = false;
float g_seat_moved = 0;
n::Vector3 g_hold_start{};

episode::Id target() { return g_episodes[g_index]; }

void result(const std::string& check, bool ok, const std::string& detail) {
  std::ofstream(g_dir / "selftest_result.txt", std::ios::app)
      << episode::short_name(target()) << '\t' << check << '\t' << (ok ? "PASS" : "FAIL") << '\t' << detail << '\n';
  log::warn("Self-test: {} {} {}: {}", episode::short_name(target()), check, ok ? "PASS" : "FAIL", detail);
}

void bot(const std::string& state) {
  std::ofstream(g_dir / "selftest_state.txt", std::ios::trunc) << "bot=" << state << '\n';
  log::info("Self-test: bot={}", state);
}

void go(Step s) { g_step = s, g_step_ms = GetTickCount(), g_mark_ms = 0; }

n::Ped me() {
  const n::Ped p = n::GetPlayerChar(n::GetPlayerId());
  return p && n::DoesCharExist(p) ? p : 0;
}

std::uint32_t my_model() {
  const n::Ped p = me();
  return p ? n::GetCharModel(p) : 0;
}

net::Mode mode() { return coop::status().session.mode; }
std::size_t players() { return coop::status().session.players.size(); }

std::string probe_text(const phone::MenuProbe& p) {
  return std::format("screen {}, items {}/{}, first id {}, Multiplayer {}", p.screen, p.count, p.capacity, p.first_id,
                     p.multiplayer ? "yes" : "no");
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

void next_episode() {
  coop::leave();
  bot("none");
  if (++g_index < g_episodes.size()) {
    go(Step::Switch);
    return;
  }
  log::warn("Self-test: done; closing the game");
  go(Step::Done);
}

} // namespace

void configure(const std::filesystem::path& plugin_dir) {
  g_dir = plugin_dir / "iv-coop";
  const auto file = g_dir / "selftest.txt";
  if (!std::filesystem::exists(file)) return;
  std::ifstream in(file);
  for (std::string line; std::getline(in, line);) {
    if (!line.starts_with("episodes=")) continue;
    std::stringstream ss(line.substr(9));
    for (std::string v; std::getline(ss, v, ',');)
      if (!v.empty() && std::isdigit(static_cast<unsigned char>(v[0]))) {
        const episode::Id id = episode::from_wire(static_cast<std::uint8_t>(std::stoi(v)));
        if (id != episode::Id::Unknown) g_episodes.push_back(id);
      }
  }
  in.close();
  std::error_code ec;
  std::filesystem::rename(file, g_dir / "selftest.active", ec);
  if (g_episodes.empty()) return;
  g_step = Step::Boot;
  log::warn("Self-test: {} episodes queued", g_episodes.size());
}

void register_hooks() {
  if (g_step == Step::Off) return;
  // While the test wants the phone open: a short "up" for the phone scripts, and a short accept (pad A, control 77
  // on pad 0 or 2; the mouse click's counterpart) for every other script, which skips the scripted intros
  // (opening_credits) and cutscenes that keep the phone away.
  native_hooks::on(kIsControlPressed, "IS_CONTROL_PRESSED", [](const Call& c) {
    if (g_step != Step::PhoneOpen || GetTickCount() - g_press_ms >= 300) return;
    const std::int32_t pad = c.arg<std::int32_t>(0), control = c.arg<std::int32_t>(1);
    std::string s = c.script;
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const bool phone_script = s.starts_with("spcellphone");
    const bool press = phone_script ? pad == 2 && control == kControlPhoneUp
                                    : (pad == 0 || pad == 2) && control == kControlSkip;
    if (press) *reinterpret_cast<std::int32_t*>(c.ctx->result) = 1;
  });
}

void tick() {
  if (g_step == Step::Off) return;
  const std::uint32_t now = GetTickCount();
  const std::uint32_t in_step = now - g_step_ms;
  static bool backgrounded = false;
  if (!backgrounded && me()) {
    backgrounded = true;
    focus::keep_running(true);
    send_window_back();
  }
  switch (g_step) {
    case Step::Boot: // whatever the game started with: settle first
      if (!me() || !episode::known()) return;
      if (!g_mark_ms) g_mark_ms = now;
      if (now - g_mark_ms > kSettleMs) bot("none"), go(Step::Switch);
      break;
    case Step::Switch:
      if (!g_mark_ms) { // once per episode
        g_mark_ms = now;
        if (episode::current() != target()) episode::request_switch(target());
        return;
      }
      if (episode::current() == target() && me()) {
        result("switch", true, std::format("in {} after {:.0f} s", episode::name(target()), in_step / 1000.0));
        go(Step::Settle);
      } else if (in_step > kSwitchTimeoutMs) {
        result("switch", false, std::format("still in {} after {} s", episode::name(episode::current()), in_step / 1000));
        next_episode();
      }
      break;
    case Step::Settle:
      if (!me()) g_step_ms = now; // the world is (re)loading
      else if (in_step > kSettleMs) {
        g_protagonist = my_model();
        result("protagonist", g_protagonist != 0,
               std::format("model 0x{:08X} ({} expected 0x{:08X})", g_protagonist, episode::protagonist_name(target()),
                           episode::protagonist_model()));
        go(Step::PhoneOpen);
      }
      break;
    case Step::PhoneOpen: {
      const phone::MenuProbe p = phone::probe();
      if (p.ui) {
        go(Step::PhoneMenu);
        phone::open_screen(kScreenMainMenu);
      } else if (in_step > kPhoneTimeoutMs) {
        result("phone", false, std::format("the phone didn't open in {} s (player control off?); {}", in_step / 1000,
                                           phone::unavailable() ? "menu marked unavailable" : "menu not marked"));
        coop::host(), bot("join"), go(Step::Host);
      } else if (now - g_press_ms > 3000) {
        g_press_ms = now;
      }
      break;
    }
    case Step::PhoneMenu:
      if (in_step < kPhoneStepMs) return;
      {
        const phone::MenuProbe p = phone::probe();
        const bool ok = p.screen == kScreenMainMenu && p.multiplayer && !phone::unavailable();
        result("phone menu", ok, probe_text(p));
        if (ok) phone::open_screen(kIdMultiplayer), go(Step::PhoneCoop);
        else phone::open_screen(kScreenClose), go(Step::PhoneClose);
      }
      break;
    case Step::PhoneCoop:
      if (in_step < kPhoneStepMs) return;
      {
        const phone::MenuProbe p = phone::probe();
        result("phone co-op items", p.screen == kScreenMainMenu && p.first_id == kIdHost, probe_text(p));
        phone::open_screen(kScreenClose);
        go(Step::PhoneClose);
      }
      break;
    case Step::PhoneClose:
      if (in_step < kPhoneStepMs) return;
      coop::host();
      bot("join");
      go(Step::Host);
      break;
    case Step::Host:
      if (mode() == net::Mode::Hosting && players() >= 2) {
        go(Step::HostHold);
      } else if (in_step > kJoinTimeoutMs) {
        result("host", false, std::format("mode {}, {} players after {} s", net::to_string(mode()), players(), in_step / 1000));
        coop::leave(), bot("serve " + std::to_string(static_cast<int>(target()))), go(Step::JoinSame);
      }
      break;
    case Step::HostHold:
      if (g_seat_bot == 0xFF && in_step > 2000)
        for (const auto& p : coop::status().session.players)
          if (p.id != coop::status().session.my_id) {
            g_seat_bot = p.id, g_seat_input_seen = false;
            activity_seats::seat(p.id, coop::puppet_ped(p.id), "selftest", -1);
            activity_seats::set_active(p.id);
          }
      if (g_seat_bot != 0xFF && activity_seats::has_input(g_seat_bot)) g_seat_input_seen = true;
      if (in_step > kHoldMs && g_seat_bot != 0xFF) {
        result("activity seat (host)", g_seat_input_seen && activity_seats::owns(coop::puppet_ped(g_seat_bot)),
               g_seat_input_seen ? "the seated bot's input arrived; its puppet is held" : "no input from the seated bot");
        activity_seats::unseat_all();
        g_seat_bot = 0xFF;
      }
      if (players() < 2) {
        result("host", false, "the bot left the session");
        coop::leave(), bot("serve " + std::to_string(static_cast<int>(target()))), go(Step::JoinSame);
      } else if (in_step > kHoldMs) {
        result("host", true, std::format("bot joined and stayed {} s; our model 0x{:08X}", kHoldMs / 1000, my_model()));
        coop::leave(), bot("serve " + std::to_string(static_cast<int>(target()))), go(Step::JoinSame);
      }
      break;
    case Step::JoinSame:
      if (in_step < kBotStartMs) return;
      if (mode() == net::Mode::Idle && !g_mark_ms) {
        if (const n::Ped p = me()) n::invoke(kRemoveWeapon, p, kBotWeapon); // so its arrival can be seen
        g_mark_ms = now, coop::join("127.0.0.1");
      }
      if (mode() == net::Mode::Joined) {
        go(Step::JoinSameHold);
        g_seated_seen = false, g_seat_moved = 0;
        if (const n::Ped p = me()) g_hold_start = n::GetCharCoordinates(p);
      } else if (in_step > kJoinTimeoutMs) {
        result("join same episode", false, std::format("mode {} after {} s", net::to_string(mode()), in_step / 1000));
        coop::leave(), go(Step::Restore);
      } else if (mode() == net::Mode::Idle && g_mark_ms && now - g_mark_ms > 8000) {
        g_mark_ms = 0; // the bot wasn't up yet: try again
      }
      break;
    case Step::JoinSameHold:
      if (activity_seats::me_seated()) g_seated_seen = true;
      if (const n::Ped p = me(); p && g_seated_seen) {
        const n::Vector3 q = n::GetCharCoordinates(p);
        g_seat_moved = std::max(g_seat_moved, std::hypot(q.x - g_hold_start.x, q.y - g_hold_start.y));
      }
      if (mode() != net::Mode::Joined) {
        result("join same episode", false, std::format("dropped after {:.0f} s (refusals {})", in_step / 1000.0,
                                                       episode_sync::refusals()));
        coop::leave(), go(Step::Restore);
      } else if (in_step > kHoldMs) {
        const std::uint32_t model = my_model();
        const episode::Id host = episode_sync::host_episode();
        result("join same episode", host == target(),
               std::format("stayed {} s; host episode {}", kHoldMs / 1000, episode::short_name(host)));
        result("client model", model != 0 && model != g_protagonist,
               std::format("0x{:08X} (protagonist 0x{:08X})", model, g_protagonist));
        const n::Ped p = me();
        result("player mirror", p && n::invoke<bool>(kHasCharGotWeapon, p, kBotWeapon),
               std::format("weapon {} from the host's story message", kBotWeapon));
        result("race checkpoint", race_sync::local_count() == 1,
               std::format("{} checkpoints from the host", race_sync::local_count()));
        result("activity seat (joiner)", g_seated_seen && g_seat_moved > 1.0f && !activity_seats::me_seated(),
               std::format("seated {}, followed the seat {:.1f} m, released {}", g_seated_seen, g_seat_moved,
                           !activity_seats::me_seated()));
        coop::leave();
        go(Step::Restore);
      }
      break;
    case Step::Restore:
      if (mode() != net::Mode::Idle) return;
      if (my_model() == g_protagonist) {
        result("model restored", true, std::format("0x{:08X} after leaving", g_protagonist));
      } else if (in_step > kRestoreTimeoutMs) {
        result("model restored", false, std::format("0x{:08X}, protagonist 0x{:08X}", my_model(), g_protagonist));
      } else {
        return;
      }
      g_refusals_before = episode_sync::refusals();
      g_joined_seen = false;
      bot("serve " + std::to_string((static_cast<int>(target()) + 1) % 3));
      go(Step::JoinOther);
      break;
    case Step::JoinOther:
      if (in_step < kBotStartMs) return;
      if (mode() == net::Mode::Joined) g_joined_seen = true;
      if (mode() == net::Mode::Idle && !g_mark_ms && !g_joined_seen) g_mark_ms = now, coop::join("127.0.0.1");
      if (episode_sync::refusals() > g_refusals_before && mode() == net::Mode::Idle) {
        result("join other episode", true, std::format("turned away after {} ms", now - g_mark_ms));
        go(Step::Next);
      } else if (in_step > kRefuseTimeoutMs) {
        result("join other episode", false,
               std::format("mode {}, refusals {} -> {}", net::to_string(mode()), g_refusals_before, episode_sync::refusals()));
        go(Step::Next);
      } else if (mode() == net::Mode::Idle && g_mark_ms && !g_joined_seen && now - g_mark_ms > 8000) {
        g_mark_ms = 0;
      }
      break;
    case Step::Next:
      if (mode() != net::Mode::Idle) coop::leave();
      if (in_step > 3000 && mode() == net::Mode::Idle && (!me() || my_model() == g_protagonist || in_step > 20000))
        next_episode();
      break;
    case Step::Done:
      if (in_step > kQuitDelayMs) TerminateProcess(GetCurrentProcess(), 0);
      break;
    case Step::Off:
      break;
  }
}

} // namespace ivc::story::self_test
