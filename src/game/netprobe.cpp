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

bool attempt_active = false;
int last_pending = -1, last_succeeded = -1;
std::uint32_t attempt_frames = 0;

bool chord(int vk, bool& was_down) {
  const bool down = (GetAsyncKeyState(vk) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                    (GetAsyncKeyState(VK_SHIFT) & 0x8000);
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  const bool edge = down && !was_down && pid == GetCurrentProcessId();
  was_down = down;
  return edge;
}

void start_attempt() {
  const auto& a = g_cfg.host_args;
  log::warn("NetProbe: attempting native LAN host: NETWORK_HOST_GAME({}, {}, {}, {}, {}, {})", a[0], a[1], a[2], a[3],
            a[4], a[5]);
  log::info("NetProbe: before: {}", describe(take()));
  invoke(kNetworkSetLanSession, 1);
  const int ok = invoke<int>(kNetworkHostGame, a[0], a[1] != 0, a[2], a[3], a[4], a[5]);
  log::info("NetProbe: NETWORK_HOST_GAME returned {}", ok);
  attempt_active = true;
  last_pending = last_succeeded = -1;
  attempt_frames = 0;
}

void follow_attempt() {
  ++attempt_frames;
  const int pending = invoke<int>(kNetworkHostGamePending);
  const int succeeded = invoke<int>(kNetworkHostGameSucceeded);
  if (pending != last_pending || succeeded != last_succeeded) {
    log::info("NetProbe: frame {}: host pending={} succeeded={}", attempt_frames, pending, succeeded);
    last_pending = pending, last_succeeded = succeeded;
  }
  if (!pending && attempt_frames > 1) {
    natives::Ped me = natives::GetPlayerChar(natives::GetPlayerId());
    int net_id = -1;
    if (me && natives::DoesCharExist(me)) invoke(kGetNetworkIdFromPed, me, &net_id);
    log::info("NetProbe: attempt settled after {} frames: succeeded={}, player network id {} | {}", attempt_frames,
              succeeded, net_id, describe(take()));
    attempt_active = false;
  } else if (attempt_frames > 60 * 120) {
    log::warn("NetProbe: still pending after ~2 min; giving up following it");
    attempt_active = false;
  }
}

} // namespace

void configure(const Settings& s) {
  g_cfg = s;
  if (s.enabled) log::warn("NetProbe enabled: Ctrl+Shift+F12 attempts a native LAN host, Ctrl+Shift+F11 leaves");
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
  if (chord(VK_F11, g_f11_down)) {
    log::warn("NetProbe: NETWORK_LEAVE_GAME -> {}", invoke<int>(kNetworkLeaveGame));
  }
  if (attempt_active) follow_attempt();
}

} // namespace ivc::netprobe
