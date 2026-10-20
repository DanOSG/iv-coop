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

// --- Extra player experiment (independent of [NetProbe] Enabled) ------------------------
// Can a second *player* (slot 1) exist in a story save? Only player peds may wear the
// protagonist model, so this is how a remote host could appear as Niko. Logs what the
// game does with it: does it copy our input, take the camera, get police attention.
constexpr std::uint32_t kSetPlayerControl = 0x1A6203EA;
constexpr std::uint32_t kSetPoliceIgnorePlayer = 0x619D51D3;
constexpr std::uint32_t kSetEveryoneIgnorePlayer = 0x059901B9;
constexpr std::uint32_t kSetPlayerInvincible = 0x7E9E02E1;
constexpr std::uint32_t kConvertIntToPlayerIndex = 0x5996315E;
constexpr std::uint32_t kPlayerHasChar = 0x22545844;
constexpr std::uint32_t kStoreWantedLevel = 0x12AA6D71;

bool g_f4_down = true, g_f5_down = true, g_f6_down = true;
natives::Player g_extra = -1;
std::uint32_t g_extra_frames = 0; // frames since it was created (follow for ~20 s)

void log_extra(const char* when) {
  using namespace natives;
  const Player me_id = GetPlayerId();
  const Ped me = GetPlayerChar(me_id);
  const Vector3 mp = GetCharCoordinates(me);
  const bool has = invoke<bool>(kPlayerHasChar, g_extra);
  Ped ped = 0;
  if (has) ped = GetPlayerChar(g_extra);
  int wanted = -1;
  invoke(kStoreWantedLevel, g_extra, &wanted);
  if (ped && DoesCharExist(ped)) {
    const Vector3 p = GetCharCoordinates(ped);
    log::info("ExtraPlayer {}: slot {} ped {} model 0x{:08X} at ({:.1f},{:.1f},{:.1f}) playing={} wanted={} | "
              "local id {} ped {} at ({:.1f},{:.1f},{:.1f}) players={}",
              when, g_extra, ped, GetCharModel(ped), p.x, p.y, p.z, IsPlayerPlaying(g_extra), wanted, me_id, me, mp.x,
              mp.y, mp.z, invoke<std::uint32_t>(kGetNumberOfPlayers));
  } else {
    log::info("ExtraPlayer {}: slot {} has_char={} ped {} (missing) | local id {} ped {}", when, g_extra, has, ped,
              me_id, me);
  }
}

// Engine internals (static VAs, GTAIV.exe 1.2.0.59). CREATE_PLAYER only (re)creates the
// *local* player and crashes if one exists, so we call what it calls, with a slot of our
// own and the "not local" flags (no camera/pad hookup).
//   create(flags, slot, model_index, flag): CPlayerInfo + player ped into slot; returns ped
//   remove(slot): deletes that slot's ped and CPlayerInfo
//   model_index(name)
constexpr std::uintptr_t kFnCreatePlayerSlot = 0x0093E960;
constexpr std::uintptr_t kFnRemovePlayerSlot = 0x0093EB70;
constexpr std::uintptr_t kFnModelIndex = 0x009DC680;
//   set_pop_type(ped this, type): CREATE_PLAYER passes 2 right after creating
//   place(ped, pos*): set position, add to the world (0x9495D0), finish setup
// CREATE_PLAYER then gives the ped its default (player-controlled) task; we skip that.
constexpr std::uintptr_t kFnSetPopType = 0x009F19C0;
constexpr std::uintptr_t kFnPlacePlayerPed = 0x0093E380;
constexpr std::uintptr_t kPlayerInfos = 0x011A8808; // CPlayerInfo* [32]
constexpr std::uint32_t kExtraSlot = 1;

template <class T> T at_static(std::uintptr_t va) {
  return reinterpret_cast<T>(va - 0x400000 + reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}

void* slot_info(std::uint32_t slot) { return at_static<void**>(kPlayerInfos)[slot]; }

void* guarded_create(std::uint32_t slot, std::int32_t model) {
  using Create = void*(__cdecl*)(const std::uint8_t*, std::uint32_t, std::int32_t, std::uint8_t);
  // Copied into the ped (+0x218/+0x219). {0,1} is what CREATE_PLAYER passes and makes the
  // slot the local one. {1,0} crashed: with +0x219 clear the ctor runs an init step that
  // needs +0x22c, which the factory only sets afterwards. {1,1} skips that step and the
  // local-player/camera hookup.
  const std::uint8_t flags[2] = {1, 1};
  __try {
    return at_static<Create>(kFnCreatePlayerSlot)(flags, slot, model, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return reinterpret_cast<void*>(-1);
  }
}

bool guarded_place(void* ped, natives::Vector3 at) {
  __try {
    at_static<void(__thiscall*)(void*, std::uint32_t)>(kFnSetPopType)(ped, 2);
    at_static<bool(__cdecl*)(void*, natives::Vector3*)>(kFnPlacePlayerPed)(ped, &at);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool guarded_remove(std::uint32_t slot) {
  __try {
    at_static<void(__cdecl*)(std::uint32_t)>(kFnRemovePlayerSlot)(slot);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void create_extra_player() {
  using namespace natives;
  if (g_extra >= 0 || slot_info(kExtraSlot)) {
    log::warn("ExtraPlayer: slot {} is already in use (remove it with Ctrl+Shift+F4)", kExtraSlot);
    return;
  }
  const Ped me = GetPlayerChar(GetPlayerId());
  const Vector3 at = GetOffsetFromCharInWorldCoords(me, {2.0f, 2.0f, 0.0f});
  const std::int32_t model = at_static<std::int32_t(__cdecl*)(const char*)>(kFnModelIndex)("player");
  log::warn("ExtraPlayer: creating slot {} (model index {}) at ({:.1f},{:.1f},{:.1f})", kExtraSlot, model, at.x, at.y,
            at.z);
  void* ped = guarded_create(kExtraSlot, model);
  if (ped == reinterpret_cast<void*>(-1)) {
    // Drop the half-built slot (leaking it) so the per-frame player update skips it.
    log::error("ExtraPlayer: the engine faulted creating the slot; clearing slot info {}", slot_info(kExtraSlot));
    at_static<void**>(kPlayerInfos)[kExtraSlot] = nullptr;
    return;
  }
  log::info("ExtraPlayer: created: ped object {}, slot info {}", ped, slot_info(kExtraSlot));
  if (!ped) return;
  log::info("ExtraPlayer: placing in the world: {}", guarded_place(ped, at) ? "ok" : "FAULTED");
  const Player p = invoke<Player>(kConvertIntToPlayerIndex, kExtraSlot);
  g_extra = p;
  g_extra_frames = 0;
  log_extra("created");
  if (invoke<bool>(kPlayerHasChar, p)) {
    const Ped h = GetPlayerChar(p);
    invoke(0x689D0F5F, h, at.x, at.y, at.z); // SET_CHAR_COORDINATES
    SetCharHeading(h, GetCharHeading(me));
  }
  // Keep it out of the single-player game: no input, no police, no hostility, no dying.
  invoke(kSetPlayerControl, p, false);
  invoke(kSetPoliceIgnorePlayer, p, true);
  invoke(kSetEveryoneIgnorePlayer, p, true);
  invoke(kSetPlayerInvincible, p, true);
  log_extra("configured");
}

void remove_extra_player() {
  if (!slot_info(kExtraSlot)) {
    log::warn("ExtraPlayer: slot {} is empty", kExtraSlot);
    return;
  }
  log::warn("ExtraPlayer: removing slot {}", kExtraSlot);
  const bool ok = guarded_remove(kExtraSlot);
  log::info("ExtraPlayer: remove {}; slot info now {}", ok ? "returned" : "FAULTED", slot_info(kExtraSlot));
  g_extra = -1;
}

void walk_extra_player() {
  using namespace natives;
  if (g_extra < 0 || !invoke<bool>(kPlayerHasChar, g_extra)) {
    log::warn("ExtraPlayer: none to move; create it first (Ctrl+Shift+F5)");
    return;
  }
  const Ped ped = GetPlayerChar(g_extra);
  const Vector3 to = GetOffsetFromCharInWorldCoords(GetPlayerChar(GetPlayerId()), {0.0f, 10.0f, 0.0f});
  log::warn("ExtraPlayer: walking slot {} to ({:.1f},{:.1f},{:.1f})", g_extra, to.x, to.y, to.z);
  TaskGoStraightToCoord(ped, to, MoveState::Walk);
  g_extra_frames = 0; // follow it again
}

void tick_extra_player() {
  if (chord(VK_F5, g_f5_down)) create_extra_player();
  if (chord(VK_F6, g_f6_down)) walk_extra_player();
  if (chord(VK_F4, g_f4_down)) remove_extra_player();
  if (g_extra >= 0 && g_extra_frames < 60 * 20) {
    if (++g_extra_frames % 60 == 0) log_extra("follow");
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
  tick_extra_player();
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
