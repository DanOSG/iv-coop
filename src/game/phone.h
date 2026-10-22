#pragma once
// The in-game phone as the co-op menu.
//
// The phone scripts (spcellphone*) keep their state in script global 91: field 0 is the
// current screen (1001 home, 1047 the Multiplayer menu, 1052 Online/LAN, ...), set by
// the scripts' screen-switch helper; the number being dialled is a string at byte 128.
// Menu labels are game-text keys, looked up through the engine's CText::Get, which we
// hook to rename items (e.g. the Multiplayer menu's match types become Host/Join Co-op).
// Engine addresses are static VAs for GTAIV.exe 1.2.0.59.

#include <cstdint>
#include <string>

namespace ivc::phone {

// Hooks the text lookup. Once, at startup.
bool install();
// Game thread, every frame: probe logging of screen and dialled-number changes.
void tick();

// Script global 91 (the phone state); empty/negative when globals aren't available.
std::int32_t screen();
std::string dialled();

// Text override: returned instead of the game's string for `key` (UTF-8, ASCII in practice).
void set_label(const std::string& key, const std::string& text);
void clear_label(const std::string& key);

} // namespace ivc::phone
