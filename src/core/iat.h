#pragma once
#include <windows.h>

namespace ivc::iat {

// Replaces `module`'s import of `dll!func` with `replacement`. Returns the previous
// pointer (the one to call through), or nullptr if the import was not found.
void* hook(HMODULE module, const char* dll, const char* func, void* replacement);

} // namespace ivc::iat
