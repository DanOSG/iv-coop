#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "sync/ghost.h"

namespace ivc {

struct Config {
  bool overlay_enabled = true;
  int overlay_toggle_key = 0x77; // VK_F8
  int scripts_panel_key = 0x76;  // VK_F7
  bool debug_log = false;
  std::vector<std::string> host_scripts = {"main"};
  std::vector<std::string> suppressed_scripts;
  ghost::Settings ghost;
};

// Reads iv-coop.ini next to the plugin, writing defaults for any missing keys.
Config load_config(const std::filesystem::path& ini);

} // namespace ivc
