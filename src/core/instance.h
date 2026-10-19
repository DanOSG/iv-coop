#pragma once
#include <string>

namespace ivc::instance {

// Claims the lowest free instance slot (0..7) via a named mutex we own. Slot 0 is the first
// copy of the game started; later copies (for local multi-instance testing) get 1, 2, ...
int claim_slot();
int slot();

// Log/file suffix for this instance: "" for slot 0, ".1", ".2", ... otherwise.
std::string suffix();

// Hooks the named-object creators GTAIV.exe imports and logs every named object it
// creates (and whether it already existed). With `isolate`, instances > 0 get the names
// rewritten with a per-instance suffix so they can't collide with the first instance.
void install_named_object_hooks(bool isolate);

} // namespace ivc::instance
