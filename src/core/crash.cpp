#include "core/crash.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <format>
#include <string>

#include "core/log.h"

namespace ivc::crash {
namespace {

std::atomic<int> g_reports{0};

std::string where(std::uintptr_t addr) {
  HMODULE mod = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(addr), &mod) ||
      !mod)
    return "??";
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(mod, path, MAX_PATH);
  std::wstring w(path);
  w = w.substr(w.find_last_of(L"\\/") + 1);
  std::string name;
  for (wchar_t ch : w) name.push_back(static_cast<char>(ch)); // module names are ASCII
  const auto base = reinterpret_cast<std::uintptr_t>(mod);
  if (mod == GetModuleHandleW(nullptr))
    return std::format("{}+0x{:X} (static VA 0x{:08X})", name, addr - base, addr - base + 0x400000);
  return std::format("{}+0x{:X}", name, addr - base);
}

bool is_code(std::uintptr_t addr) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) return false;
  return mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                                    PAGE_EXECUTE_WRITECOPY));
}

LONG CALLBACK handler(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case 0xC0000409: // stack buffer overrun / fail-fast
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
  if (g_reports.fetch_add(1) >= 5) return EXCEPTION_CONTINUE_SEARCH; // first-chance storms: report a few

  const auto addr = reinterpret_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
  const CONTEXT* c = ep->ContextRecord;
  std::string detail;
  if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
    const auto kind = ep->ExceptionRecord->ExceptionInformation[0];
    detail = std::format(" ({} of 0x{:08X})", kind == 0 ? "read" : kind == 1 ? "write" : "execute",
                         ep->ExceptionRecord->ExceptionInformation[1]);
  }
  log::error("EXCEPTION 0x{:08X}{} at 0x{:08X} in {} (thread {})", code, detail, addr, where(addr),
             GetCurrentThreadId());
  log::error("  eax={:08X} ebx={:08X} ecx={:08X} edx={:08X} esi={:08X} edi={:08X} ebp={:08X} esp={:08X}", c->Eax,
             c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
  // Heuristic stack: code pointers on the stack (may include stale return addresses).
  int shown = 0;
  for (int i = 0; i < 1024 && shown < 16; ++i) {
    const auto* slot = reinterpret_cast<const std::uintptr_t*>(c->Esp) + i;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(slot, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
    const std::uintptr_t v = *slot;
    if (v > 0x10000 && is_code(v)) {
      log::error("  [esp+0x{:03X}] 0x{:08X} {}", i * 4, v, where(v));
      ++shown;
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install() {
  if (AddVectoredExceptionHandler(1, handler))
    log::info("Crash logger installed");
  else
    log::warn("Crash logger: AddVectoredExceptionHandler failed");
}

} // namespace ivc::crash
