#pragma once
// One zip with everything needed to look into a problem, put on the Desktop: the mod's logs (this launch and the one
// before, so a crash's log survives a restart), its ini, the self-test/runner/coverage results, the game's d3d9 log
// and FusionFix ini, and a summary (mod version, game folder, time). No saves, nothing personal beyond the player name
// in the ini. Built with Windows' own tar.exe. Shared by the mod (phone: Multiplayer > Save logs, or Ctrl+Shift+L)
// and the installer (Collect logs), so it works when the game won't start too.

#include <filesystem>
#include <string>

namespace ivc::log_export {

struct Result {
  bool ok = false;
  std::filesystem::path zip; // on success
  std::string message;       // what happened, for the player
};

// `game_dir` is the folder with GTAIV.exe. Blocks (a second or two); call off the game thread.
Result run(const std::filesystem::path& game_dir, const std::string& mod_version);

} // namespace ivc::log_export
