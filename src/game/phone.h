#pragma once
// The in-game phone as the co-op menu.
//
// CE removed the phone's Multiplayer item; we put one back. The phone scripts keep their
// state in script global 91 (field 0 the current screen, set by each script's screen-
// switch helper; the number being dialled is a string at byte 128). The main menu
// (screen 1011, script spcellphonemain) is a list in that script's statics; selecting an
// item switches the phone to the screen whose id is the item's id. So:
//   - on 1011 we append "Multiplayer" (id 2000);
//   - selecting it lands on screen 2000: we send the phone back to 1011 with our own items
//     (Host Co-op 2001 / Join Co-op 2002, or Leave Co-op 2003 while in a session);
//   - Host/Join open the phone's keypad (screen 1003) with a hint to type a 6-digit
//     password and press Call; when the phone starts calling (1005) the digits are the password
//     (the call to an unknown number then hangs up by itself).
// Labels are game-text keys; CText::Get is hooked to supply ours. Engine addresses and
// script variable slots are for GTAIV.exe 1.2.0.59 (CE); see phone.cpp.

#include <cstdint>
#include <optional>
#include <string>

namespace ivc::phone {

enum class Action { Host, Join, Leave, GoToHost, Invade, SaveLogs };
struct Request {
  Action action;
  std::string password; // 6 digits (Host/Join)
};

// Hooks the text lookup. Once, at startup.
bool install();
// Game thread, script context, every frame.
void tick();
// The player's latest menu choice, once complete (password typed for Host/Join).
std::optional<Request> poll();
// The menu shows Host/Join/Invade when not in a session, Leave for the host, Go to Host +
// Leave for a joiner, and just Leave for an invader (M11: Invade finds a lobby open to invasion).
enum class Role { None, Host, Joiner, Invader };
void set_role(Role role);
// A short message in the help box (e.g. "Hosting co-op, password 123456").
void notify(const std::string& text);

// Script global 91 (the phone state); -1/empty when globals aren't available.
std::int32_t screen();
std::string dialled();

// True when the co-op menu can't be put on the phone: the episode isn't known yet, or the main menu's list didn't
// match the known script layout (logged). remote_players then offers F10 host / F11 join instead.
bool unavailable();

// Self-test (story/self_test): the phone's main-menu list as the script holds it, and a jump to a screen the way the
// phone's own switch helper does it (1011 main menu, 2000 our Multiplayer item, 1001 close).
struct MenuProbe {
  std::int32_t screen = -1, count = -1, capacity = -1, first_id = -1;
  bool multiplayer = false; // our item is in the list
  bool ui = false;          // spcellphonemain is running
};
MenuProbe probe();
void open_screen(std::int32_t screen);

// Text override: returned instead of the game's string for `key` (ASCII).
void set_label(const std::string& key, const std::string& text);

} // namespace ivc::phone
