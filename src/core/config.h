#pragma once
#include <filesystem>

namespace ivc {

struct Config {
  bool overlay_enabled = true;
  int overlay_toggle_key = 0x77; // VK_F8
  bool debug_log = false;
};

// Reads iv-coop.ini next to the plugin, writing defaults for any missing keys.
Config load_config(const std::filesystem::path& ini);

} // namespace ivc
