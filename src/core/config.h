#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "sync/ghost.h"
#include "sync/remote_players.h"

namespace ivc {

struct Config {
  bool overlay_enabled = true;
  int overlay_toggle_key = 0x77; // VK_F8
  int scripts_panel_key = 0x76;  // VK_F7
  bool debug_log = false;
  bool isolate_named_objects = false; // local multi-instance testing; see core/instance.h
  std::vector<std::string> host_scripts = {"main"};
  std::vector<std::string> suppressed_scripts;
  ghost::Settings ghost;
  coop::Settings net;
};

// Reads iv-coop.ini next to the plugin, writing defaults for any missing keys.
Config load_config(const std::filesystem::path& ini);

} // namespace ivc
