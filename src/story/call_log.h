#pragma once

namespace ivc::story {

// M6a diagnostics: log every story-relevant native call scripts make (text, blips,
// entity creation, script launches, cutscenes, conversations), with the calling script,
// arguments and results. Registers native hooks; call before native_hooks::install().
void register_call_logging();

} // namespace ivc::story
