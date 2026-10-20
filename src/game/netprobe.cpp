#include "game/netprobe.h"

#include <windows.h>

#include <format>
#include <string>

#include "core/log.h"
#include "game/addresses.h"
#include "game/natives.h"

namespace ivc::netprobe {
namespace {
using natives::invoke;

// Native hashes (community native list, as used by FusionFix).
constexpr std::uint32_t kIsNetworkSession = 0x6E2B38F3;
constexpr std::uint32_t kNetworkIsNetworkAvailable = 0x04E11812;
constexpr std::uint32_t kIsNetworkGameRunning = 0x1CF773D4;
constexpr std::uint32_t kIsNetworkGamePending = 0x7563071D;
constexpr std::uint32_t kNetworkGetGameMode = 0x29A75D1F;
constexpr std::uint32_t kNetworkIsSessionStarted = 0x65B83AFB;
constexpr std::uint32_t kNetworkIsOperationPending = 0x71AE456A;
constexpr std::uint32_t kNetworkSetLanSession = 0x6FDA43A3;
constexpr std::uint32_t kNetworkHostGame = 0x5BEA05E2;
constexpr std::uint32_t kNetworkHostGamePending = 0x391E4575;
constexpr std::uint32_t kNetworkHostGameSucceeded = 0x1CA77E94;
constexpr std::uint32_t kNetworkLeaveGame = 0x55D66E24;
constexpr std::uint32_t kNetworkStartSession = 0x58802CE5;
constexpr std::uint32_t kNetworkStartSessionPending = 0x7F853FF4;
constexpr std::uint32_t kNetworkStartSessionSucceeded = 0x5873667B;
constexpr std::uint32_t kGetNetworkIdFromPed = 0x7BEE5003;
constexpr std::uint32_t kGetNumberOfPlayers = 0x62405882;

// Game-mode globals (static VAs in GTAIV.exe 1.2.0.59, image base 0x400000; plain .data).
// current mode, pending mode, change-pending flag. IS_NETWORK_SESSION is (current == 1).
constexpr std::uintptr_t kModeCurrent = 0x011D6FC4;
constexpr std::uintptr_t kModePending = 0x011D6FC8;
constexpr std::uintptr_t kModeChangeFlag = 0x011D6FCC;

std::int32_t read_global(std::uintptr_t static_va) {
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  return *reinterpret_cast<const std::int32_t*>(static_va - 0x400000 + base);
}

int script_count() {
  const game::ThreadArray* arr = game::g_addr.threads;
  int n = 0;
  for (std::uint16_t i = 0; arr && arr->data && i < arr->count; ++i)
    if (arr->data[i] && *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(arr->data[i]) + 4))
      ++n;
  return n;
}

struct Snapshot {
  int session, available, running, pending, game_mode, started, op_pending, players;
  int mode_cur, mode_pend, mode_flag, scripts;
  bool operator==(const Snapshot&) const = default;
};

Snapshot take() {
  return {invoke<int>(kIsNetworkSession),        invoke<int>(kNetworkIsNetworkAvailable),
          invoke<int>(kIsNetworkGameRunning),    invoke<int>(kIsNetworkGamePending),
          invoke<int>(kNetworkGetGameMode),      invoke<int>(kNetworkIsSessionStarted) & 0xFF,
          invoke<int>(kNetworkIsOperationPending), static_cast<int>(invoke<std::uint32_t>(kGetNumberOfPlayers)),
          read_global(kModeCurrent),             read_global(kModePending),
          read_global(kModeChangeFlag),          script_count()};
}

std::string describe(const Snapshot& s) {
  return std::format("session={} available={} running={} pending={} game_mode={} started={} op_pending={} "
                     "players={} | mode cur={} pend={} flag={} | scripts={}",
                     s.session, s.available, s.running, s.pending, s.game_mode, s.started, s.op_pending, s.players,
                     s.mode_cur, s.mode_pend, s.mode_flag, s.scripts);
}

Settings g_cfg;
Snapshot g_last{};
bool g_have_last = false;
bool g_f12_down = true, g_f11_down = true;

bool g_f10_down = true;

// The operation being followed frame by frame (host or start-session).
struct Attempt {
  const char* what = nullptr;
  std::uint32_t pending_hash = 0, succeeded_hash = 0;
  int last_pending = -1, last_succeeded = -1;
  std::uint32_t frames = 0;
};
bool attempt_active = false;
Attempt g_attempt;

void follow(const char* what, std::uint32_t pending_hash, std::uint32_t succeeded_hash) {
  g_attempt = {what, pending_hash, succeeded_hash};
  attempt_active = true;
}

bool chord(int vk, bool& was_down) {
  const bool down = (GetAsyncKeyState(vk) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                    (GetAsyncKeyState(VK_SHIFT) & 0x8000);
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  const bool edge = down && !was_down && pid == GetCurrentProcessId();
  was_down = down;
  return edge;
}

bool g_hosted = false; // a host attempt succeeded in this process

void start_attempt() {
  // Hosting again while a session exists hangs the game thread (seen in run 2: the
  // engine spins forever inside the call). The MP frontend never does that; neither do we.
  const Snapshot now = take();
  if (now.running || now.started || now.pending || attempt_active) {
    log::warn("NetProbe: refusing to host: a session already exists or is pending ({})", describe(now));
    return;
  }
  const auto& a = g_cfg.host_args;
  log::warn("NetProbe: attempting native LAN host: NETWORK_HOST_GAME({}, {}, {}, {}, {}, {})", a[0], a[1], a[2], a[3],
            a[4], a[5]);
  log::info("NetProbe: before: {}", describe(take()));
  invoke(kNetworkSetLanSession, 1);
  const int ok = invoke<int>(kNetworkHostGame, a[0], a[1] != 0, a[2], a[3], a[4], a[5]);
  log::info("NetProbe: NETWORK_HOST_GAME returned {}", ok);
  follow("host", kNetworkHostGamePending, kNetworkHostGameSucceeded);
}

// The step the MP frontend takes after hosting; suspected to switch the game into
// network mode (IS_NETWORK_SESSION / mode globals), possibly resetting the world.
void start_session() {
  if (!g_hosted || attempt_active) {
    log::warn("NetProbe: refusing to start session: host first (Ctrl+Shift+F12) and wait for it to succeed");
    return;
  }
  log::warn("NetProbe: NETWORK_START_SESSION");
  log::info("NetProbe: before: {}", describe(take()));
  const int ok = invoke<int>(kNetworkStartSession);
  log::info("NetProbe: NETWORK_START_SESSION returned {}", ok);
  follow("start session", kNetworkStartSessionPending, kNetworkStartSessionSucceeded);
}

void follow_attempt() {
  Attempt& a = g_attempt;
  ++a.frames;
  const int pending = invoke<int>(a.pending_hash);
  const int succeeded = invoke<int>(a.succeeded_hash);
  if (pending != a.last_pending || succeeded != a.last_succeeded) {
    log::info("NetProbe: {} frame {}: pending={} succeeded={}", a.what, a.frames, pending, succeeded);
    a.last_pending = pending, a.last_succeeded = succeeded;
  }
  if (!pending && a.frames > 1) {
    natives::Ped me = natives::GetPlayerChar(natives::GetPlayerId());
    int net_id = -1;
    if (me && natives::DoesCharExist(me)) invoke(kGetNetworkIdFromPed, me, &net_id);
    log::info("NetProbe: {} settled after {} frames: succeeded={}, player network id {} | {}", a.what, a.frames,
              succeeded, net_id, describe(take()));
    if (a.pending_hash == kNetworkHostGamePending && succeeded) g_hosted = true;
    attempt_active = false;
  } else if (a.frames > 60 * 120) {
    log::warn("NetProbe: {} still pending after ~2 min; giving up following it", a.what);
    attempt_active = false;
  }
}

} // namespace

void configure(const Settings& s) {
  g_cfg = s;
  if (s.enabled)
    log::warn("NetProbe enabled: Ctrl+Shift+F12 native LAN host, Ctrl+Shift+F10 start session, "
              "Ctrl+Shift+F11 leave");
}

void tick() {
  if (!g_cfg.enabled) return;

  const Snapshot s = take();
  if (!g_have_last || !(s == g_last)) {
    log::info("NetProbe: {}", describe(s));
    g_last = s;
    g_have_last = true;
  }

  if (chord(VK_F12, g_f12_down)) start_attempt();
  if (chord(VK_F10, g_f10_down)) start_session();
  if (chord(VK_F11, g_f11_down)) {
    log::warn("NetProbe: NETWORK_LEAVE_GAME -> {}", invoke<int>(kNetworkLeaveGame));
  }
  if (attempt_active) follow_attempt();
}

} // namespace ivc::netprobe
