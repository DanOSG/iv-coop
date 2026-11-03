#include "sync/activity_seats.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <vector>

#include "core/log.h"
#include "game/native_hooks.h"
#include "game/script.h"
#include "net/bytes.h"

namespace ivc::activity_seats {
namespace {
namespace n = natives;
using native_hooks::Call;

constexpr std::uint32_t kGetSticks = 0x4F7F4FAE, kGetMouse = 0x447B154B, kGetKeyboardMove = 0x4AF73456,
                        kIsControlPressed = 0x0E635761, kIsControlJustPressed = 0x4CB729F1,
                        kIsUsingController = 0x669D053F, kIsButtonPressed = 0x7FF21081,
                        kIsMouseButtonPressed = 0x39E600D0, kSetPlayerControl = 0x1A6203EA,
                        kRequestAnims = 0x65F874DE,
                        kHaveAnimsLoaded = 0x1D3F681D, kDeleteChar = 0x0E3B49BF, kMarkNoLongerNeeded = 0x0B774604,
                        kRemoveCharElegantly = 0x5731084A, kGetPlayerChar = 0x511454A9;
constexpr DWORD kPoseMs = 66, kInputMs = 33, kInputStaleMs = 400, kSeatResendMs = 2000, kSeatStaleMs = 6000;
constexpr float kPoseSnapM = 0.35f, kPoseTurnDeg = 8.0f;
constexpr std::uint8_t kNobody = 0xFF;

enum Op : std::uint8_t { OpSeat = 0, OpPose = 1, OpAnim = 2, OpInput = 3 };

// The control reads relayed: (pad, control). Index = bit in the masks. Covers the activities' scripts (pool, darts,
// bowling, golf, air hockey, arm wrestling, cards): aim/shoot/accept/cancel/camera and the d-pad style controls.
struct Ctl {
  int pad, control;
};
constexpr Ctl kCtl[] = {{2, 0},  {2, 1},  {2, 2},  {2, 3},  {2, 4},   {2, 21}, {2, 22}, {2, 23},
                        {2, 77}, {2, 78}, {2, 87}, {2, 181}, {2, 182}, {0, 23}, {0, 77}, {0, 78},
                        // hi-lo cards' higher / lower (keyboard, controller): appended, the indices above are fixed
                        {2, 93}, {2, 94}, {2, 79}, {2, 130}, {2, 131}};
constexpr int kNumCtl = static_cast<int>(sizeof(kCtl) / sizeof(kCtl[0]));

int ctl_index(int pad, int control) {
  for (int i = 0; i < kNumCtl; ++i)
    if (kCtl[i].pad == pad && kCtl[i].control == control) return i;
  return -1;
}

struct Input {
  std::uint32_t controls = 0, just = 0, buttons = 0;
  std::uint8_t mouse_buttons = 0;
  bool controller = false;
  std::int16_t lx = 0, ly = 0, rx = 0, ry = 0, mx = 0, my = 0, kx = 0, ky = 0;
  DWORD at = 0;
  bool fresh() const { return at && GetTickCount() - at < kInputStaleMs; }
};

// One captured animation native: which argument slots are strings, which one names the dictionary.
struct AnimSpec {
  std::uint32_t hash;
  const char* name;
  std::uint32_t strings;
  int dict;
};
constexpr AnimSpec kAnims[] = {
    {0x28EE78D8, "TASK_PLAY_ANIM", 0b110, 2},
    {0x71F001D2, "TASK_PLAY_ANIM_FACIAL", 0b110, 2},
    {0x52202E76, "TASK_PLAY_ANIM_NON_INTERRUPTABLE", 0b110, 2},
    {0x273C2D35, "TASK_PLAY_ANIM_SECONDARY", 0b110, 2},
    {0x56524B94, "TASK_PLAY_ANIM_SECONDARY_NO_INTERRUPT", 0b110, 2},
    {0x34574B2A, "TASK_PLAY_ANIM_SECONDARY_UPPER_BODY", 0b110, 2},
    {0x02534709, "TASK_PLAY_ANIM_UPPER_BODY", 0b110, 2},
    {0x30BA2716, "TASK_PLAY_ANIM_WITH_ADVANCED_FLAGS", 0b110, 2},
    {0x75533E74, "TASK_PLAY_ANIM_WITH_FLAGS", 0b110, 2},
    {0x1A122D03, "TASK_PLAY_ANIM_WITH_FLAGS_AND_START_PHASE", 0b110, 2},
};
constexpr int kMaxAnimArgs = 12;

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

// ---- host state
struct Seat {
  n::Ped ped = 0;
  std::string script; // lower case
  int pad = -1;
  n::Ped stand_in = 0;
  DWORD last_pose = 0, last_seat = 0;
  bool dirty = true;
};
bool g_hosting = false;
std::uint8_t g_my_id = 0;
std::map<std::uint8_t, Seat> g_seats;
std::uint8_t g_active = kNobody;
std::map<std::uint8_t, Input> g_inputs;
std::vector<std::vector<std::uint8_t>> g_out;

// ---- partner state
bool g_me_seated = false, g_me_active = false;
DWORD g_seat_at = 0, g_sent_at = 0;
std::uint32_t g_pending_just = 0;
struct PendingAnim {
  std::uint32_t hash;
  std::uint8_t argc;
  std::uint32_t args[kMaxAnimArgs];
  std::uint32_t strings;
  std::string text[kMaxAnimArgs];
  std::string dict;
  DWORD since;
};
std::vector<PendingAnim> g_anims;

n::Ped player_ped() {
  const n::Ped p = n::GetPlayerChar(n::GetPlayerId());
  return p && n::DoesCharExist(p) ? p : 0;
}

// Host: whose input answers this read. Pad reads go to the seat on that pad; generic reads (pad 0 / 2, or no pad)
// from the active seat's script go to the active partner.
const Input* input_for(const Call& c, int pad) {
  if (!g_hosting || g_seats.empty()) return nullptr;
  if (pad >= 0)
    for (const auto& [id, s] : g_seats)
      if (s.pad >= 0 && s.pad == pad && s.pad != 0 && s.pad != 2) {
        const auto it = g_inputs.find(id);
        return it != g_inputs.end() ? &it->second : nullptr;
      }
  if (g_active == kNobody) return nullptr;
  const auto seat = g_seats.find(g_active);
  if (seat == g_seats.end() || lower(c.script) != seat->second.script) return nullptr;
  if (pad > 2) return nullptr; // another seat's own pad, not the generic one
  const auto it = g_inputs.find(g_active);
  static const Input kIdle{};
  return it != g_inputs.end() ? &it->second : &kIdle; // the active partner hasn't sent anything: nothing pressed
}

void put_out(net::Writer&& w) {
  if (g_out.size() < 256) g_out.push_back(std::move(w.data()));
}

net::Writer msg(Op op, std::uint8_t target) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(static_cast<std::uint8_t>(op)).put(target);
  return w;
}

// ---- host hooks: answer the script's input reads with the seated partner's

void on_sticks(const Call& c) {
  const Input* in = input_for(c, c.arg<std::int32_t>(0));
  if (!in) return;
  const bool f = in->fresh();
  const std::int32_t v[4] = {f ? in->lx : 0, f ? in->ly : 0, f ? in->rx : 0, f ? in->ry : 0};
  for (int i = 0; i < 4; ++i)
    if (auto* p = c.arg<std::int32_t*>(1 + i)) *p = v[i];
}

void on_pair(const Call& c, bool mouse) {
  const Input* in = input_for(c, -1);
  if (!in) return;
  const bool f = in->fresh();
  if (auto* p = c.arg<std::int32_t*>(0)) *p = f ? (mouse ? in->mx : in->kx) : 0;
  if (auto* p = c.arg<std::int32_t*>(1)) *p = f ? (mouse ? in->my : in->ky) : 0;
}

void on_control(const Call& c, bool just) {
  const int pad = c.arg<std::int32_t>(0), i = ctl_index(pad, c.arg<std::int32_t>(1));
  if (i < 0) return;
  const Input* cin = input_for(c, pad);
  if (!cin) return;
  auto* in = const_cast<Input*>(cin);
  bool v;
  if (just) {
    v = (in->just & (1u << i)) != 0;
    in->just &= ~(1u << i);
  } else {
    v = in->fresh() && (in->controls & (1u << i));
  }
  *reinterpret_cast<std::int32_t*>(c.ctx->result) = v ? 1 : 0;
}

void on_button(const Call& c) {
  const int b = c.arg<std::int32_t>(1);
  if (b < 0 || b > 31) return;
  const Input* in = input_for(c, c.arg<std::int32_t>(0));
  if (!in) return;
  *reinterpret_cast<std::int32_t*>(c.ctx->result) = in->fresh() && (in->buttons & (1u << b)) ? 1 : 0;
}

void on_mouse_button(const Call& c) {
  const int b = c.arg<std::int32_t>(0);
  if (b < 0 || b > 7) return;
  const Input* in = input_for(c, -1);
  if (!in) return;
  *reinterpret_cast<std::int32_t*>(c.ctx->result) = in->fresh() && (in->mouse_buttons & (1u << b)) ? 1 : 0;
}

void on_using_controller(const Call& c) {
  const Input* in = input_for(c, -1);
  if (!in) return;
  *reinterpret_cast<std::int32_t*>(c.ctx->result) = in->fresh() && in->controller ? 1 : 0;
}

// Host: the script animates a seated partner's puppet: the partner's own character plays it too.
void on_anim(const Call& c, const AnimSpec& spec) {
  if (!g_hosting || g_seats.empty()) return;
  const auto ped = c.arg<n::Ped>(0);
  for (const auto& [id, s] : g_seats) {
    if (s.ped != ped) continue;
    const int argc = std::min(c.arg_count, kMaxAnimArgs);
    auto w = msg(OpAnim, id);
    w.put(spec.hash).put(static_cast<std::uint8_t>(argc)).put(spec.strings);
    for (int a = 0; a < argc; ++a) {
      if (spec.strings & (1u << a)) {
        const std::string t = c.str(a);
        w.put(static_cast<std::uint8_t>(std::min<std::size_t>(t.size(), 63)));
        for (std::size_t k = 0; k < std::min<std::size_t>(t.size(), 63); ++k) w.put(static_cast<std::uint8_t>(t[k]));
      } else {
        w.put(c.arg<std::uint32_t>(a));
      }
    }
    w.put(static_cast<std::int8_t>(spec.dict));
    put_out(std::move(w));
    return;
  }
}

// Host: the script is about to delete or release a ped. A seated puppet is swapped for its seat's stand-in (the
// pointer forms write the script's variable, so it ends up holding nothing, as it expects).
n::Ped stand_in_for(n::Ped ped) {
  for (const auto& [id, s] : g_seats)
    if (s.ped == ped && ped) return s.stand_in ? s.stand_in : -1;
  return 0;
}

void before_release(const Call& c, bool by_pointer) {
  if (!g_hosting || g_seats.empty()) return;
  auto* args = static_cast<std::uint32_t*>(c.ctx->args);
  n::Ped* var = by_pointer ? reinterpret_cast<n::Ped*>(static_cast<std::uintptr_t>(args[0])) : nullptr;
  const n::Ped ped = by_pointer ? (var ? *var : 0) : static_cast<n::Ped>(args[0]);
  const n::Ped other = stand_in_for(ped);
  if (!other) return;
  const n::Ped target = other == -1 ? 0 : other; // no stand-in: aim it at nothing
  if (by_pointer) *var = target;
  else args[0] = static_cast<std::uint32_t>(target);
  log::info("Activity: {} on a seated partner went to {}", c.name, target ? "the stand-in" : "nothing");
}

// Host: on a seated partner's turn the active script's "the player" is that partner's puppet. Golf, bowling and the
// table games key everything (attachments, animations, run-ups, room checks) on GET_PLAYER_CHAR.
void on_player_char(const Call& c) {
  if (!g_hosting || g_active == kNobody) return;
  const auto seat = g_seats.find(g_active);
  if (seat == g_seats.end() || !seat->second.ped || lower(c.script) != seat->second.script) return;
  if (auto* out = c.arg<n::Ped*>(1)) *out = seat->second.ped;
}

void send_seat(std::uint8_t id, const Seat& s, bool on, net::Session& net) {
  auto w = msg(OpSeat, id);
  // A seat on its own pad (> 2) is live all the time: its script reads that pad (or the adapter reads sample()).
  w.put(static_cast<std::uint8_t>(on)).put(static_cast<std::uint8_t>(on && (g_active == id || s.pad > 2)))
      .put(static_cast<std::int8_t>(s.pad));
  net.send_game(std::move(w.data()), true);
}

// ---- partner

void send_input(net::Session& net, DWORD now) {
  std::int32_t lx = 0, ly = 0, rx = 0, ry = 0, mx = 0, my = 0, kx = 0, ky = 0;
  n::invoke(kGetSticks, 0, &lx, &ly, &rx, &ry);
  n::invoke(kGetMouse, &mx, &my);
  n::invoke(kGetKeyboardMove, &kx, &ky);
  std::uint32_t controls = 0, just = 0, buttons = 0;
  for (int i = 0; i < kNumCtl; ++i) {
    if (n::invoke<bool>(kIsControlPressed, kCtl[i].pad, kCtl[i].control)) controls |= 1u << i;
    if (n::invoke<bool>(kIsControlJustPressed, kCtl[i].pad, kCtl[i].control)) just |= 1u << i;
  }
  for (int b = 0; b <= 19; ++b)
    if (n::invoke<bool>(kIsButtonPressed, 0, b)) buttons |= 1u << b;
  std::uint8_t mouse_buttons = 0;
  for (int b = 1; b <= 2; ++b)
    if (n::invoke<bool>(kIsMouseButtonPressed, b)) mouse_buttons |= std::uint8_t(1u << b);
  g_pending_just |= just;
  if (now - g_sent_at < kInputMs && !just) return;
  g_sent_at = now;
  auto s16 = [](std::int32_t v) { return static_cast<std::int16_t>(std::clamp(v, -32768, 32767)); };
  auto w = msg(OpInput, g_my_id);
  w.put(controls).put(g_pending_just).put(buttons).put(mouse_buttons)
      .put(static_cast<std::uint8_t>(n::invoke<bool>(kIsUsingController) ? 1 : 0))
      .put(s16(lx)).put(s16(ly)).put(s16(rx)).put(s16(ry)).put(s16(mx)).put(s16(my)).put(s16(kx)).put(s16(ky));
  net.send_game(std::move(w.data()), g_pending_just != 0); // a button edge must not get lost
  g_pending_just = 0;
}

void set_seated(bool on) {
  if (on == g_me_seated) return;
  g_me_seated = on;
  n::invoke(kSetPlayerControl, n::GetPlayerId(), !on);
  if (!on) g_me_active = false, g_anims.clear();
  log::info("Activity: {} the host's game", on ? "seated in" : "left");
}

void play_pending_anims() {
  const n::Ped me = player_ped();
  if (!me) return;
  const DWORD now = GetTickCount();
  for (auto it = g_anims.begin(); it != g_anims.end();) {
    if (now - it->since > 3000) { // the dictionary never loaded
      it = g_anims.erase(it);
      continue;
    }
    if (!it->dict.empty() && !n::invoke<bool>(kHaveAnimsLoaded, it->dict.c_str())) {
      n::invoke(kRequestAnims, it->dict.c_str());
      ++it;
      continue;
    }
    std::uint32_t args[kMaxAnimArgs];
    for (int a = 0; a < it->argc; ++a)
      args[a] = (it->strings & (1u << a)) ? static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(it->text[a].c_str()))
                                          : it->args[a];
    args[0] = static_cast<std::uint32_t>(me);
    n::invoke_raw(it->hash, args, it->argc);
    it = g_anims.erase(it);
  }
}

} // namespace

void seat(std::uint8_t id, n::Ped ped, const std::string& script, int pad) {
  Seat& s = g_seats[id];
  s.ped = ped, s.script = lower(script), s.pad = pad, s.dirty = true;
  log::info("Activity: player {} seated in {} (ped {}, pad {})", id, script, ped, pad);
}

void unseat(std::uint8_t id) {
  const auto it = g_seats.find(id);
  if (it == g_seats.end()) return;
  // Tell them now; the seat is gone from the map before the next tick.
  auto w = msg(OpSeat, id);
  w.put(std::uint8_t{0}).put(std::uint8_t{0}).put(std::int8_t{-1});
  put_out(std::move(w));
  if (g_active == id) g_active = kNobody;
  g_seats.erase(it);
  g_inputs.erase(id);
  log::info("Activity: player {} unseated", id);
}

void unseat_all() {
  std::vector<std::uint8_t> ids;
  for (const auto& [id, s] : g_seats) ids.push_back(id);
  for (auto id : ids) unseat(id);
}

void set_active(std::uint8_t id) {
  if (g_active == id) return;
  g_active = id;
  for (auto& [sid, s] : g_seats) s.dirty = true;
}

void set_stand_in(std::uint8_t id, n::Ped stand_in) {
  if (const auto it = g_seats.find(id); it != g_seats.end()) it->second.stand_in = stand_in;
}

std::int32_t* statics(const char* script) {
  auto* t = static_cast<std::uint8_t*>(script::find_thread(script));
  return t ? *reinterpret_cast<std::int32_t**>(t + 0x58) : nullptr;
}

bool any_seated() { return g_hosting && !g_seats.empty(); }

bool has_input(std::uint8_t id) {
  const auto it = g_inputs.find(id);
  return it != g_inputs.end() && it->second.fresh() && (it->second.controls & (1u << 8)); // (2, 77) held
}

bool seated(std::uint8_t id) { return g_seats.count(id) != 0; }

PadSample sample(std::uint8_t id) {
  PadSample out;
  const auto it = g_inputs.find(id);
  if (it == g_inputs.end() || !it->second.fresh()) return out;
  const Input& in = it->second;
  out.fresh = true, out.controller = in.controller;
  out.mx = in.mx, out.my = in.my, out.kx = in.kx, out.ky = in.ky;
  return out;
}

bool control(std::uint8_t id, int pad, int control, bool edge) {
  const int i = ctl_index(pad, control);
  const auto it = g_inputs.find(id);
  if (i < 0 || it == g_inputs.end()) return false;
  Input& in = it->second;
  if (edge) {
    const bool v = (in.just & (1u << i)) != 0;
    in.just &= ~(1u << i);
    return v;
  }
  return in.fresh() && (in.controls & (1u << i));
}

bool owns(n::Ped ped) {
  if (!ped || !g_hosting) return false;
  for (const auto& [id, s] : g_seats)
    if (s.ped == ped) return true;
  return false;
}

bool me_seated() { return g_me_seated; }

void register_hooks() {
  using native_hooks::on;
  on(kGetSticks, "GET_POSITION_OF_ANALOGUE_STICKS", on_sticks);
  on(kGetMouse, "GET_MOUSE_INPUT", [](const Call& c) { on_pair(c, true); });
  on(kGetKeyboardMove, "GET_KEYBOARD_MOVE_INPUT", [](const Call& c) { on_pair(c, false); });
  on(kIsControlPressed, "IS_CONTROL_PRESSED", [](const Call& c) { on_control(c, false); });
  on(kIsControlJustPressed, "IS_CONTROL_JUST_PRESSED", [](const Call& c) { on_control(c, true); });
  on(kIsButtonPressed, "IS_BUTTON_PRESSED", on_button);
  on(kIsMouseButtonPressed, "IS_MOUSE_BUTTON_PRESSED", on_mouse_button);
  on(kIsUsingController, "IS_USING_CONTROLLER", on_using_controller);
  for (const AnimSpec& a : kAnims) on(a.hash, a.name, [&a](const Call& c) { on_anim(c, a); });
  on(kGetPlayerChar, "GET_PLAYER_CHAR", on_player_char);
  native_hooks::before(kDeleteChar, "DELETE_CHAR", [](const Call& c) { before_release(c, true); });
  native_hooks::before(kMarkNoLongerNeeded, "MARK_CHAR_AS_NO_LONGER_NEEDED", [](const Call& c) { before_release(c, true); });
  native_hooks::before(kRemoveCharElegantly, "REMOVE_CHAR_ELEGANTLY", [](const Call& c) { before_release(c, false); });
}

void tick(bool in_session, bool hosting, std::uint8_t my_id, net::Session& net) {
  const DWORD now = GetTickCount();
  g_my_id = my_id;
  if (g_hosting && !(in_session && hosting)) g_seats.clear(), g_inputs.clear(), g_active = kNobody;
  g_hosting = in_session && hosting;
  if (!in_session) {
    set_seated(false);
    g_out.clear();
    return;
  }
  for (auto& d : g_out) net.send_game(std::move(d), true);
  g_out.clear();
  if (g_hosting) {
    for (auto it = g_seats.begin(); it != g_seats.end();) {
      Seat& s = it->second;
      if (!s.ped || !n::DoesCharExist(s.ped)) { // the puppet went (the partner left)
        const std::uint8_t id = it->first;
        ++it;
        unseat(id);
        continue;
      }
      if (s.dirty || now - s.last_seat > kSeatResendMs) {
        send_seat(it->first, s, true, net);
        s.dirty = false, s.last_seat = now;
      }
      if (now - s.last_pose > kPoseMs) {
        s.last_pose = now;
        const n::Vector3 p = n::GetCharCoordinates(s.ped);
        const float heading = n::GetCharHeading(s.ped);
        auto w = msg(OpPose, it->first);
        w.put(p.x).put(p.y).put(p.z).put(heading);
        net.send_game(std::move(w.data()), false);
      }
      ++it;
    }
    return;
  }
  if (g_me_seated && now - g_seat_at > kSeatStaleMs) set_seated(false); // the host stopped saying we're seated
  if (g_me_seated) play_pending_anims();
  if (g_me_seated && g_me_active) send_input(net, now);
}

void on_message(const net::GameMessage& m) {
  net::Reader r(m.data);
  std::uint8_t type, sender, op, target;
  if (!r.get(type) || !r.get(sender) || !r.get(op) || !r.get(target)) return;
  if (op == OpInput) {
    if (!g_hosting || !g_seats.count(m.from)) return;
    Input in;
    std::uint32_t just;
    std::uint8_t controller;
    if (!r.get(in.controls) || !r.get(just) || !r.get(in.buttons) || !r.get(in.mouse_buttons) || !r.get(controller) ||
        !r.get(in.lx) || !r.get(in.ly) || !r.get(in.rx) || !r.get(in.ry) || !r.get(in.mx) || !r.get(in.my) ||
        !r.get(in.kx) || !r.get(in.ky))
      return;
    in.controller = controller != 0;
    Input& slot = g_inputs[m.from];
    in.just = slot.just | just;
    in.at = GetTickCount();
    slot = in;
    return;
  }
  if (m.from != net::kHostId || g_hosting || target != g_my_id) return;
  if (op == OpSeat) {
    std::uint8_t on, active;
    std::int8_t pad;
    if (!r.get(on) || !r.get(active) || !r.get(pad)) return;
    set_seated(on != 0);
    g_me_active = on && active;
    g_seat_at = GetTickCount();
  } else if (op == OpPose) {
    float x, y, z, heading;
    if (!g_me_seated || !r.get(x) || !r.get(y) || !r.get(z) || !r.get(heading)) return;
    const n::Ped me = player_ped();
    if (!me) return;
    const n::Vector3 cur = n::GetCharCoordinates(me);
    if (std::hypot(cur.x - x, cur.y - y) > kPoseSnapM || std::fabs(cur.z - z) > 1.0f)
      n::SetCharCoordinatesNoOffset(me, {x, y, z});
    const float mine = n::GetCharHeading(me);
    float d = std::fmod(std::fabs(mine - heading), 360.0f);
    if (d > 180.0f) d = 360.0f - d;
    if (d > kPoseTurnDeg) n::SetCharHeading(me, heading);
  } else if (op == OpAnim) {
    if (!g_me_seated) return;
    PendingAnim a{};
    std::uint8_t argc;
    if (!r.get(a.hash) || !r.get(argc) || !r.get(a.strings) || argc > kMaxAnimArgs) return;
    a.argc = argc;
    for (int i = 0; i < argc; ++i) {
      if (a.strings & (1u << i)) {
        std::uint8_t len;
        if (!r.get(len)) return;
        for (int k = 0; k < len; ++k) {
          std::uint8_t ch;
          if (!r.get(ch)) return;
          a.text[i] += static_cast<char>(ch);
        }
      } else if (!r.get(a.args[i])) {
        return;
      }
    }
    std::int8_t dict;
    if (!r.get(dict)) return;
    if (dict >= 0 && dict < argc) a.dict = a.text[dict];
    a.since = GetTickCount();
    if (g_anims.size() < 16) g_anims.push_back(std::move(a));
  }
}

} // namespace ivc::activity_seats
