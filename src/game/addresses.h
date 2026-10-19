#pragma once
#include <cstdint>

namespace ivc::game {

struct scrThread; // opaque; field access goes through script.h helpers

// rage::sysArray<scrThread*> as laid out in GTAIV.exe 1.2.0.59.
struct ThreadArray {
  scrThread** data;
  std::uint16_t count;
  std::uint16_t capacity;
};

using NativeHandler = void(__cdecl*)(void* ctx);
using GetNativeHandler_t = NativeHandler(__stdcall*)(std::uint32_t hash);

struct Addresses {
  GetNativeHandler_t get_native_handler = nullptr; // rage::scrEngine native hash table lookup
  scrThread** current_thread = nullptr;            // rage::scrThread::sm_pActiveThread
  ThreadArray* threads = nullptr;                  // rage::scrThread::sm_Threads
  void* thread_run = nullptr;                      // rage::scrThread::Run (bytecode interpreter)
  void* update_all = nullptr;                      // rage::scrThread::UpdateAll (once per frame)
};

inline Addresses g_addr;

// Resolves everything by pattern. Logs each address; returns false if any is missing,
// in which case the game layer must stay disabled.
bool resolve_addresses();

} // namespace ivc::game
