// iv-coop: free-roam co-op for GTA IV: The Complete Edition.
// Loaded by Ultimate ASI Loader (dinput8.dll) from <game>/plugins/iv-coop.asi.

#include <windows.h>

#include <MinHook.h>

#include <filesystem>

#include "core/config.h"
#include "core/game_version.h"
#include "core/log.h"
#include "render/d3d9_hook.h"
#include "render/overlay.h"

namespace {

std::filesystem::path plugin_dir(HMODULE self) {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(self, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path();
}

// Runs inside DllMain, so it does only loader-lock-safe work: file IO, an IAT
// write, and MinHook init. All vtable detours happen later on the game thread.
void startup(HMODULE self) {
  using namespace ivc;
  const auto dir = plugin_dir(self);
  log::init(dir / "iv-coop.log");
  const Config cfg = load_config(dir / "iv-coop.ini");
  if (cfg.debug_log) log::set_min_level(log::Level::Debug);
  log::info("iv-coop M0 loading (built " __DATE__ " " __TIME__ ")");

  const GameVersion ver = query_game_version();
  log::info("Host exe version {}", ver.str());
  if (ver != kSupportedVersion)
    log::warn("Untested game version (expected {}); expect breakage", kSupportedVersion.str());

  if (const MH_STATUS st = MH_Initialize(); st != MH_OK) {
    log::error("MH_Initialize failed: {}", MH_StatusToString(st));
    return;
  }

  overlay::configure(cfg.overlay_enabled, cfg.overlay_toggle_key);
  d3d9_hook::install();
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
