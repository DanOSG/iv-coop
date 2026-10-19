#include "game/natives.h"

#include <unordered_map>
#include <unordered_set>

#include "core/log.h"
#include "game/addresses.h"

namespace ivc::natives {

bool available() { return game::g_addr.get_native_handler != nullptr; }

// Only touched from the game thread.
void* handler(std::uint32_t hash) {
  static std::unordered_map<std::uint32_t, void*> cache;
  static std::unordered_set<std::uint32_t> reported;
  if (!available()) return nullptr;

  if (auto it = cache.find(hash); it != cache.end()) return it->second;
  void* fn = reinterpret_cast<void*>(game::g_addr.get_native_handler(hash));
  if (!fn) {
    if (reported.insert(hash).second) log::error("Native 0x{:08X} not found", hash);
    return nullptr;
  }
  cache.emplace(hash, fn);
  return fn;
}

} // namespace ivc::natives
