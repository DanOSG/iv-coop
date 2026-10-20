#pragma once
// Native interception (M6): watch the calls story scripts make to chosen natives.
//
// Each registered native's handler gets a detour that runs the original and then the
// callbacks, with access to the call's arguments and results (including values written
// through pointer arguments, e.g. the blip a script just created). Calls made by our own
// code (our tick, or natives called from inside a callback) are not reported.

#include <cstdint>
#include <functional>
#include <string>

#include "game/natives.h"

namespace ivc::native_hooks {

struct Call {
  natives::CallContext* ctx;
  std::uint32_t hash;
  const char* name;
  std::string script; // calling script thread

  template <class T>
  T arg(int i) const {
    return *reinterpret_cast<const T*>(static_cast<const std::uint8_t*>(ctx->args) + 4 * i);
  }
  // Value written through a pointer argument (e.g. Blip* out).
  template <class T>
  T out(int i) const {
    const T* p = arg<const T*>(i);
    return p ? *p : T{};
  }
  template <class T>
  T result() const {
    return *reinterpret_cast<const T*>(ctx->result);
  }
};

using Callback = std::function<void(const Call&)>;

// Register before install(). Several callbacks per native are fine.
void on(std::uint32_t hash, const char* name, Callback cb);
// Game thread (natives must be registered by then; call from the first tick). Idempotent.
bool install();

} // namespace ivc::native_hooks
