#pragma once
// MP-style character creator (ARCHITECTURE, M9).
//
// A character is a model (M_Y_MULTIPLAYER or F_Y_MULTIPLAYER), a drawable + texture for components 0..8, and
// two props (hat, glasses). It is saved per user in plugins/iv-coop/character.ini and worn by the joiner's own
// ped (instead of the plain joiner_model), edited from the phone (Multiplayer > Character) with a live preview
// on the player's own ped, and sent to the other players (message 61) so each machine dresses that player's
// copy the same way.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "game/natives.h"
#include "net/session.h"

namespace ivc::character {

constexpr std::uint8_t kMsg = 61;
constexpr int kComponents = 9; // head, uppr, lowr, suse, hand, feet, jacket, hair, suse2
constexpr int kProps = 2;      // hat, glasses

struct Character {
  bool female = false;
  std::uint8_t drawable[kComponents] = {};
  std::uint8_t texture[kComponents] = {};
  std::int8_t prop[kProps] = {-1, -1}; // -1: none
  std::uint8_t prop_tex[kProps] = {};
  bool operator==(const Character&) const = default;
};

// Loads character.ini (plugin_dir/iv-coop/character.ini). Once, at startup.
void configure(const std::filesystem::path& plugin_dir);

// --- our own character (the saved one) ---
bool configured();                                  // a character was saved
std::uint32_t model_hash(const Character& c);
// The model the joiner plays as: the saved character's, or `fallback` (the configured joiner_model).
std::uint32_t joiner_model(std::uint32_t fallback);
bool is_own_model(std::uint32_t model);             // configured() and it is the saved character's model
// Dresses the player's ped (just given the model) in the saved character.
void apply_own(natives::Ped ped);

// --- editor (phone) ---
bool editor_active();
bool editor_open();                                 // false: no player ped
void editor_close(bool save);                       // Back (false) / Save (true); the editor ends either way
std::vector<std::string> editor_rows();             // the phone's list, without Save / Back
void editor_activate(int row);                      // select on a row: next value
bool editor_take_dirty();                           // the rows changed on their own (a model finished loading)

// --- sync ---
struct Frame {
  bool in_session = false;
  bool joiner_active = false; // a joiner whose own ped wears their character: sends it
  std::uint8_t my_id = 0;
  std::vector<std::uint8_t> players; // ids in the session
};
// Game thread, every frame: finishes editor model swaps, sends our character.
void tick(const Frame& f, net::Session& net);
inline bool is_msg(std::span<const std::uint8_t> d) { return !d.empty() && d[0] == kMsg; }
void on_message(const net::GameMessage& m);
// A player's copy exists (call every frame it does): dresses it when their character arrived or the ped is new.
void on_puppet(std::uint8_t id, natives::Ped ped);
void forget(std::uint8_t id);

} // namespace ivc::character
