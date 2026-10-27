#pragma once
// Co-op session settings (phone: Multiplayer > Co-op Settings).
//
// The host's settings are the session's: the host (or a player before hosting) changes them in the
// phone menu, they're saved to iv-coop.ini [Session], and the host sends them to joiners every few
// seconds and on every change. Everything that depends on one reads current().

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "net/session.h"

namespace ivc::session_settings {

enum class FriendlyFire : std::uint8_t {
  Off = 0,        // partners can't be locked on to and take no damage from each other
  On = 1,         // lock-on and damage
  DamageOnly = 2, // no lock-on, but shots and blows still hurt
};

struct Settings {
  FriendlyFire friendly_fire = FriendlyFire::DamageOnly; // the default: damage, no lock-on
  bool downed = true;         // downed/revive on missions (off: deaths are normal)
  bool shared_payouts = true; // joiners get the host's mission payouts
  bool radio_sync = true;
  bool world_sync = true;     // clock and weather follow the host
  bool partner_blips = true;
  bool ambient_sync = false;  // city peds and traffic near several players are shared (off until proven in play)
  bool operator==(const Settings&) const = default;
};

constexpr std::uint8_t kMsg = 44;

// The settings in force: the host's while joined, our own otherwise.
const Settings& current();
inline bool targeting_allowed() { return current().friendly_fire == FriendlyFire::On; }
inline bool damage_allowed() { return current().friendly_fire != FriendlyFire::Off; }

// Loads our own settings (iv-coop.ini [Session]). Once, at startup.
void load(const std::filesystem::path& ini);

// The phone menu: one entry per setting; cycle(i) steps setting i to its next value and saves.
int count();
std::string label(int i);          // e.g. "Friendly fire: Off"
std::string label_key(int i);      // a game-text key unique to the setting's current value
void all_labels(void (*add)(const std::string& key, const std::string& text));
void cycle(int i);

// Game thread, every frame: the host sends, a joiner switches to the host's settings.
void tick(bool in_session, bool hosting, net::Session& net);
inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }
void on_message(const net::GameMessage& m);

} // namespace ivc::session_settings
