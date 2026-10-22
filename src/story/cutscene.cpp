#include "story/cutscene.h"

#include <windows.h>

#include <optional>
#include <string>

#include "core/log.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "net/bytes.h"
#include "story/story_protocol.h"
#include "story/story_sync.h"

namespace ivc::story::cutscene {
namespace {
namespace n = natives;
using n::invoke;
using native_hooks::Call;

constexpr std::uint32_t kStartNow = 0x53591DD7, kInit = 0x47E50BD3, kStart = 0x5F752F19, kStop = 0x50FF1428,
                        kClearNamed = 0x62EF058E, kHasFinished = 0x4ECE1AD2;
constexpr DWORD kSettleMs = 1000;       // HAS_CUTSCENE_FINISHED isn't meaningful right after a start
constexpr DWORD kMaxMs = 10 * 60 * 1000; // safety: no cutscene lasts this long

Callbacks g_cb;

// ---------------------------------------------------------------- host
bool g_hosting = false;
std::string g_init_name;                 // INIT_CUTSCENE's name, until START_CUTSCENE
std::optional<std::string> g_host_started; // to send
bool g_host_active = false;
DWORD g_host_since = 0;

bool story_call(const Call& c) { return g_hosting && is_story_script(c.script); }

void send(net::Session& net, StoryMsg type, const std::string& name) {
  net::Writer w;
  w.put(type).put(std::uint8_t{0}).put(make_key(name));
  net.send_game(std::move(w.data()), true);
}

// ---------------------------------------------------------------- joiner
enum class State { Idle, Becoming, Playing, Restoring };
State g_state = State::Idle;
std::string g_name;
std::optional<std::string> g_pending; // started by the host, not yet playing here
bool g_host_ended = false, g_stopped = false;
DWORD g_since = 0;

} // namespace

void set_callbacks(Callbacks cb) { g_cb = std::move(cb); }

void register_hooks() {
  using native_hooks::on;
  on(kStartNow, "START_CUTSCENE_NOW", [](const Call& c) {
    if (story_call(c)) g_host_started = c.str(0);
  });
  on(kInit, "INIT_CUTSCENE", [](const Call& c) {
    if (story_call(c)) g_init_name = c.str(0);
  });
  on(kStart, "START_CUTSCENE", [](const Call& c) {
    if (story_call(c) && !g_init_name.empty()) g_host_started = g_init_name;
  });
}

void host_tick(bool hosting, net::Session& net) {
  g_hosting = hosting;
  if (!hosting) {
    g_host_started.reset();
    g_host_active = false;
    return;
  }
  const DWORD now = GetTickCount();
  if (g_host_started) {
    send(net, StoryMsg::CutsceneStart, *g_host_started);
    log::info("Cutscene: host started {}", *g_host_started);
    g_host_started.reset();
    g_host_active = true, g_host_since = now;
  }
  if (g_host_active && now - g_host_since > kSettleMs && invoke<bool>(kHasFinished)) {
    g_host_active = false;
    send(net, StoryMsg::CutsceneEnd, "");
    log::info("Cutscene: host's cutscene ended after {} s", (now - g_host_since) / 1000);
  }
}

void client_apply(std::span<const std::uint8_t> msg) {
  net::Reader r(msg);
  StoryMsg type;
  std::uint8_t sender;
  Key name{};
  if (!r.get(type) || !r.get(sender) || !r.get(name)) return;
  if (type == StoryMsg::CutsceneStart) {
    g_pending = to_string(name);
    g_host_ended = false;
  } else if (type == StoryMsg::CutsceneEnd) {
    g_host_ended = true;
    if (g_state == State::Idle) g_pending.reset(); // over before we could start it
  }
}

void client_tick() {
  const DWORD now = GetTickCount();
  switch (g_state) {
    case State::Idle:
      if (g_pending) {
        g_name = *g_pending;
        g_pending.reset();
        g_state = State::Becoming;
      }
      break;
    case State::Becoming:
      if (g_cb.begin && !g_cb.begin()) break;
      invoke(kStartNow, g_name.c_str());
      g_state = State::Playing, g_since = now, g_stopped = false;
      log::info("Cutscene: playing {}", g_name);
      break;
    case State::Playing:
      if ((g_host_ended || now - g_since > kMaxMs) && !g_stopped) {
        invoke(kStop); // like a skip; the cutscene then reports finished
        g_stopped = true;
      }
      if (now - g_since > kSettleMs && invoke<bool>(kHasFinished)) {
        invoke(kClearNamed, g_name.c_str()); // as the scripts do, once it has finished
        g_state = State::Restoring;
        log::info("Cutscene: {} finished{}", g_name, g_stopped ? " (stopped with the host)" : "");
      }
      break;
    case State::Restoring:
      if (g_cb.end && !g_cb.end()) break;
      g_state = State::Idle;
      g_host_ended = false;
      break;
  }
}

bool playing() { return g_host_active || g_state == State::Playing; }

void clear() {
  g_host_started.reset();
  g_host_active = false;
  g_pending.reset();
  if (g_state == State::Playing) {
    invoke(kStop);
    g_state = State::Restoring; // client_tick finishes the restore
  }
}

} // namespace ivc::story::cutscene
