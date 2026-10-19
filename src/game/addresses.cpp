#include "game/addresses.h"

#include "core/log.h"
#include "game/pattern.h"

// Patterns were located in GTAIV.exe 1.2.0.59 with tools/find_pattern.py + dumpbin /disasm.
// Static addresses for reference (image base 0x400000):
//   scrThread::Run            0x0086D0A0
//   scrThread::GetActiveThread 0x0086D060  -> sm_pActiveThread 0x01BB54DC
//   program lookup            0x0086CCD0  (same hash-table shape as natives; NOT what we want)
//   native lookup             0x0086FCE0  -> table 0x01B4AF1C, size 0x01B4AF20
//   sm_Threads                0x01B4AF04
//   scrThread::UpdateAll      0x0086E630  (called once from 0x0094DEA9)
//   vtables: rage::scrThread 0x00FE7EB0, GtaThread 0x00EB3CCC
//            [0] dtor [1] Reset [2] Run [3] Update [4] Kill

namespace ivc::game {
namespace {

bool expect(const char* what, const std::vector<std::uintptr_t>& hits, size_t want) {
  if (hits.size() == want) return true;
  log::error("Pattern for {} matched {} time(s), expected {}", what, hits.size(), want);
  return false;
}

} // namespace

bool resolve_addresses() {
  bool ok = true;

  // Two identical hash-table lookups: [0] resolves script programs, [1] resolves natives.
  if (auto h = pattern::find_all("56 8B 35 ? ? ? ? 85 F6 75 06 33 C0 5E C2 04 00 53 57 8B 7C 24 10");
      expect("native lookup", h, 2))
    g_addr.get_native_handler = reinterpret_cast<GetNativeHandler_t>(h[1]);
  else
    ok = false;

  // GetActiveThread: `mov eax, [sm_pActiveThread]; ret`, directly followed by scrThread::Kill.
  if (auto h = pattern::find_all("A1 ? ? ? ? C3 CC CC CC CC CC CC CC CC CC CC 8B 44 24 04 56 8B F1 89 46 6C");
      expect("active thread", h, 1))
    g_addr.current_thread = pattern::read<scrThread**>(h[0] + 1);
  else
    ok = false;

  if (auto h = pattern::find_all("8B 15 ? ? ? ? 5E 85 D2 74 ? 8B 03"); expect("thread array", h, 1))
    g_addr.threads = pattern::read<ThreadArray*>(h[0] + 2);
  else
    ok = false;

  if (auto h = pattern::find_all("55 8B EC 83 E4 F0 81 EC E8 00 00 00 56 57 8B F9 89 7C 24 3C 8B 47 0C");
      expect("scrThread::Run", h, 1))
    g_addr.thread_run = reinterpret_cast<void*>(h[0]);
  else
    ok = false;

  // Loops over sm_Threads calling each thread's Update (vtable +0x0C); one caller, once a frame.
  if (auto h = pattern::find_all("66 8B 15 ? ? ? ? 53 56 57 8B 7C 24 10 32 DB B8"); expect("scrThread::UpdateAll", h, 1))
    g_addr.update_all = reinterpret_cast<void*>(h[0]);
  else
    ok = false;

  log::info("Game addresses: native_lookup={} active_thread={} threads={} run={} update_all={}",
            reinterpret_cast<void*>(g_addr.get_native_handler), static_cast<void*>(g_addr.current_thread),
            static_cast<void*>(g_addr.threads), g_addr.thread_run, g_addr.update_all);
  return ok;
}

} // namespace ivc::game
