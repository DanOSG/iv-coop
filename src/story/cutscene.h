#pragma once
// Cutscenes for joiners (M7).
//
// The host's mission starts a cutscene (START_CUTSCENE_NOW, or INIT_CUTSCENE + START_CUTSCENE);
// the host sends its name, and later that it ended (played out or skipped). Each joiner
// plays the same cutscene locally, wherever they are. IV casts the local player as the
// protagonist in cutscenes, so for the duration the joiner's player is switched back to
// the protagonist's model (and their position restored afterwards); other players'
// puppets are hidden on every machine while a cutscene plays. If the host's cutscene ends
// first, the joiner's is stopped like a skip (STOP_CUTSCENE) and cleared once finished.

#include <cstdint>
#include <functional>
#include <span>

#include "net/session.h"

namespace ivc::story::cutscene {

// Joiner-side hooks into the player layer. Each returns true once done (they may need a
// few frames, e.g. to stream a model).
struct Callbacks {
  std::function<bool()> begin; // become the protagonist, remember where we were
  std::function<bool()> end;   // back to the joiner character, where we were
};
void set_callbacks(Callbacks cb);

void register_hooks();
void host_tick(bool hosting, net::Session& net);
void client_apply(std::span<const std::uint8_t> msg);
void client_tick();
// A cutscene is playing here (host: the mission's; joiner: our replay): hide other players.
bool playing();
void clear();

} // namespace ivc::story::cutscene
