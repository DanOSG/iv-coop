#include "core/instance.h"

#include <windows.h>

#include <format>

#include "core/iat.h"
#include "core/log.h"

namespace ivc::instance {
namespace {

int g_slot = 0;
bool g_isolate = false;

using CreateMutexA_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
using CreateEventA_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
using CreateSemaphoreA_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCSTR);
using CreateFileMappingA_t = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);

CreateMutexA_t o_CreateMutexA = nullptr;
CreateEventA_t o_CreateEventA = nullptr;
CreateSemaphoreA_t o_CreateSemaphoreA = nullptr;
CreateFileMappingA_t o_CreateFileMappingA = nullptr;

// Returns the name to actually use; `storage` keeps a rewritten name alive.
LPCSTR rename(LPCSTR name, std::string& storage) {
  if (!name || !g_isolate || g_slot == 0) return name;
  storage = std::format("{}_ivc{}", name, g_slot);
  return storage.c_str();
}

// Logs without clobbering the last-error value the game is about to check.
void report(const char* kind, LPCSTR name, LPCSTR used, HANDLE h) {
  const DWORD err = GetLastError();
  if (name)
    log::info("{} '{}'{} -> {}{}", kind, name, used != name ? std::format(" as '{}'", used) : std::string(),
              static_cast<void*>(h), err == ERROR_ALREADY_EXISTS ? " (ALREADY EXISTS)" : "");
  SetLastError(err);
}

HANDLE WINAPI hk_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR name) {
  std::string s;
  LPCSTR used = rename(name, s);
  HANDLE h = o_CreateMutexA(sa, owner, used);
  report("CreateMutexA", name, used, h);
  return h;
}

HANDLE WINAPI hk_CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name) {
  std::string s;
  LPCSTR used = rename(name, s);
  HANDLE h = o_CreateEventA(sa, manual, initial, used);
  report("CreateEventA", name, used, h);
  return h;
}

HANDLE WINAPI hk_CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG max, LPCSTR name) {
  std::string s;
  LPCSTR used = rename(name, s);
  HANDLE h = o_CreateSemaphoreA(sa, initial, max, used);
  report("CreateSemaphoreA", name, used, h);
  return h;
}

HANDLE WINAPI hk_CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo,
                                    LPCSTR name) {
  std::string s;
  LPCSTR used = rename(name, s);
  HANDLE h = o_CreateFileMappingA(file, sa, prot, hi, lo, used);
  report("CreateFileMappingA", name, used, h);
  return h;
}

template <class T>
void hook(const char* func, T replacement, T& original) {
  void* prev = iat::hook(GetModuleHandleW(nullptr), "kernel32.dll", func, reinterpret_cast<void*>(replacement));
  if (prev)
    original = reinterpret_cast<T>(prev);
  else
    log::warn("Import kernel32!{} not found; named-object tracking incomplete", func);
}

} // namespace

int claim_slot() {
  for (int i = 0; i < 8; ++i) {
    const std::wstring name = std::format(L"Local\\iv-coop-instance-{}", i);
    HANDLE h = CreateMutexW(nullptr, FALSE, name.c_str());
    if (h && GetLastError() != ERROR_ALREADY_EXISTS) {
      g_slot = i; // handle intentionally kept open for the life of the process
      return i;
    }
    if (h) CloseHandle(h);
  }
  g_slot = 7;
  return g_slot;
}

int slot() { return g_slot; }

std::string suffix() { return g_slot == 0 ? std::string() : std::format(".{}", g_slot); }

void install_named_object_hooks(bool isolate) {
  g_isolate = isolate;
  hook("CreateMutexA", &hk_CreateMutexA, o_CreateMutexA);
  hook("CreateEventA", &hk_CreateEventA, o_CreateEventA);
  hook("CreateSemaphoreA", &hk_CreateSemaphoreA, o_CreateSemaphoreA);
  hook("CreateFileMappingA", &hk_CreateFileMappingA, o_CreateFileMappingA);
}

} // namespace ivc::instance
