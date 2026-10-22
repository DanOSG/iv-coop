// iv-coop: free-roam co-op for GTA IV: The Complete Edition.
// Loaded by Ultimate ASI Loader (dinput8.dll) from <game>/plugins/iv-coop.asi.

#include <windows.h>

#include <MinHook.h>

#include <filesystem>

#include "core/config.h"
#include "core/crash.h"
#include "core/game_version.h"
#include "core/instance.h"
#include "core/log.h"
#include "game/addresses.h"
#include "game/native_hooks.h"
#include "game/netprobe.h"
#include "story/call_log.h"
#include "story/mission_audit.h"
#include "story/story_sync.h"
#include "game/script.h"
#include "render/d3d9_hook.h"
#include "render/overlay.h"
#include "sync/ghost.h"
#include "sync/remote_players.h"

namespace {

std::filesystem::path plugin_dir(HMODULE self) {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(self, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path();
}

// Brings up the script layer: pattern-resolved addresses, the scrThread::Run hook, and the
// per-frame game-thread tick. Disabled (with a log line) if anything doesn't resolve.
void start_game_layer(const ivc::Config& cfg, bool version_ok) {
  using namespace ivc;
  if (!version_ok) {
    log::warn("Game layer disabled: patterns are only verified for {}", kSupportedVersion.str());
    return;
  }
  if (!game::resolve_addresses() || !script::install()) {
    log::error("Game layer disabled: address resolution or script hook failed");
    return;
  }
  script::set_host_scripts(cfg.host_scripts);
  for (const auto& name : cfg.suppressed_scripts) script::suppress(name, true);
  ghost::configure(cfg.ghost);
  coop::configure(cfg.net);
  netprobe::configure(cfg.netprobe);
  if (cfg.story_log_calls) story::register_call_logging();
  story::configure(cfg.story);
  story::register_hooks();
  story::audit::configure(cfg.audit);
  story::audit::register_hooks();
  native_hooks::set_trace(cfg.story_trace_calls);
  // Natives are registered by the time any script runs. Hook them before the scripts'
  // first update, not in our first tick (which runs after them): a game that loads a
  // save straight away has "main" re-apply the world in that very first update.
  script::on_frame_start([] {
    static bool hooked = false;
    if (!hooked) hooked = native_hooks::install();
  });
  script::on_tick([] {
    ghost::tick();
    coop::tick();
    netprobe::tick();
    story::audit::tick();
  });
}

// Runs inside DllMain, before the game's own startup: file IO, an IAT write, and
// MinHook detours on code that hasn't run yet. Nothing here may wait on another thread.
void startup(HMODULE self) {
  using namespace ivc;
  const auto dir = plugin_dir(self);
  const int slot = instance::claim_slot();
  log::init(dir / ("iv-coop" + instance::suffix() + ".log"));
  const Config cfg = load_config(dir / "iv-coop.ini");
  if (cfg.debug_log) log::set_min_level(log::Level::Debug);
  log::info("iv-coop M2 loading (built " __DATE__ " " __TIME__ "), instance slot {}, pid {}", slot,
            GetCurrentProcessId());
  {
    wchar_t* cmd = GetCommandLineW();
    std::string narrow;
    for (const wchar_t* c = cmd; *c; ++c) narrow.push_back(static_cast<char>(*c));
    log::info("Command line: {}", narrow);
  }
  instance::install_named_object_hooks(cfg.isolate_named_objects);
  crash::install();

  const GameVersion ver = query_game_version();
  log::info("Host exe version {}", ver.str());
  const bool version_ok = ver == kSupportedVersion;
  if (!version_ok) log::warn("Untested game version (expected {}); expect breakage", kSupportedVersion.str());

  if (const MH_STATUS st = MH_Initialize(); st != MH_OK) {
    log::error("MH_Initialize failed: {}", MH_StatusToString(st));
    return;
  }

  overlay::configure(cfg.overlay_enabled, cfg.overlay_toggle_key, cfg.scripts_panel_key);
  d3d9_hook::install();
  start_game_layer(cfg, version_ok);
  log::info("Startup complete");
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    startup(module);
  }
  // No teardown on detach: the process is exiting and the game thread may still be in our hooks.
  return TRUE;
}
