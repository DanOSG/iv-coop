#pragma once
// Story replication (M6b/c).
//
// Scripts are either *local* (ambient: ATMs, vendors, help prompts...; they run on every
// machine and are left alone) or *story* (missions and story flow: main, speechcontrol,
// romanmarker, roman2, ...). Story scripts run only on the host; what they do that players
// must see is captured on the host and replayed on clients:
//   - on-screen text and help (by game-text key),
//   - blips (coordinate/contact/entity) and their colour/sprite/scale/display/route/name,
//   - the peds and vehicles they create, which become networked entities pinned to the
//     host (their AI is the host script's), and are removed when the script deletes or
//     releases them.

#include <string>
#include <vector>

#include "net/session.h"
#include "sync/entity_sync.h"

namespace ivc::story {

struct Settings {
  bool replicate = true;
  // Wildcard patterns (only '*') for scripts that run locally everywhere.
  std::vector<std::string> local_scripts = {"amb*", "vendor*", "foodserver*", "binco*", "*help", "carfood*"};
};

void configure(const Settings& s);
// Registers the capture hooks; call before native_hooks::install().
void register_hooks();
bool is_story_script(const std::string& name);

// --- per frame, game thread, from the co-op tick ---
// Host: send what story scripts did since last frame (nothing is captured unless hosting).
void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents);
// Client: apply a story message from the host (returns false if it wasn't one).
bool client_on_message(const net::GameMessage& m, sync::EntitySync& ents);
// Client: retry blips waiting for their entity's local copy to spawn.
void client_tick(sync::EntitySync& ents);
// Session ended: remove everything we created for the host's story.
void clear();

struct Status {
  unsigned texts = 0, blips_live = 0, entities_registered = 0, messages = 0;
};
Status status();

} // namespace ivc::story
