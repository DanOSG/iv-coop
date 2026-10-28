#pragma once
// Elden Ring style event banners ("FBI INVADING", "FBI VANQUISHED", ...): a wide translucent dark band across the
// screen centre with large spaced serif capitals, fading in, holding and fading out, with a stinger.
//
// Font: Cinzel (SIL OFL), embedded in the .asi as a resource. Sound: an optional WAV in plugins\iv-coop\ui\
// (banner_invading.wav, banner_victory.wav, banner_defeat.wav, played async), else a game sound.

#include <filesystem>

#include <windows.h>

namespace ivc::banner {

enum class Kind { Invading, Victory, Defeat };

void configure(HMODULE self, const std::filesystem::path& plugin_dir);
// Game thread: show a banner (replaces one on screen) and play its sound.
void show(Kind kind, const char* text);
bool active();
// Render thread: load_font once after the ImGui context exists and before the backend builds the atlas; draw
// inside an ImGui frame.
void load_font();
void draw();

} // namespace ivc::banner
