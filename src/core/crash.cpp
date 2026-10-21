#include "core/crash.h"

#include <windows.h>
#include <psapi.h>

#include <crtdbg.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <format>
#include <string>
#include <thread>

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

void log_stack(std::uintptr_t esp, int max_frames) {
  // Heuristic stack: code pointers on the stack (may include stale return addresses).
  int shown = 0;
  for (int i = 0; i < 1024 && shown < max_frames; ++i) {
    const auto* slot = reinterpret_cast<const std::uintptr_t*>(esp) + i;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(slot, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
    const std::uintptr_t v = *slot;
    if (v > 0x10000 && is_code(v)) {
      log::error("  [esp+0x{:03X}] 0x{:08X} {}", i * 4, v, where(v));
      ++shown;
    }
  }
}

// --- hang watchdog: the game thread (tick) and render thread (Present) beat; if both
// stop for kHangAfter, log where each one is. A frozen fullscreen game otherwise leaves
// nothing behind.
using Clock = std::chrono::steady_clock;
std::atomic<std::int64_t> g_last_beat{0};
std::atomic<DWORD> g_tick_thread{0}, g_render_thread{0};
constexpr auto kHangAfter = std::chrono::seconds(8);

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void dump_thread(const char* what, DWORD tid) {
  if (!tid) return;
  HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!t) return;
  CONTEXT c{};
  c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  bool ok = false;
  if (SuspendThread(t) != static_cast<DWORD>(-1)) {
    ok = GetThreadContext(t, &c) != 0;
    ResumeThread(t); // never log while it is suspended (it may hold the log lock)
  }
  CloseHandle(t);
  if (!ok) return;
  log::error("HANG: {} thread {} at 0x{:08X} in {}", what, tid, c.Eip, where(c.Eip));
  log_stack(c.Esp, 20);
}

void watchdog() {
  bool reported = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const std::int64_t last = g_last_beat.load();
    if (!last) continue;
    const bool hung = now_ms() - last > std::chrono::duration_cast<std::chrono::milliseconds>(kHangAfter).count();
    if (hung && !reported) {
      reported = true;
      log::error("HANG: no game tick or frame for {} s", kHangAfter.count());
      dump_thread("game", g_tick_thread.load());
      dump_thread("render", g_render_thread.load());
    } else if (!hung) {
      reported = false;
    }
  }
}

// Debug-runtime checks (bad iterators, out-of-range...) show a message box, which hides
// behind a fullscreen game and looks like a freeze: log them instead.
int crt_report(int type, char* msg, int* ret) {
  log::error("CRT {}: {}", type == _CRT_ASSERT ? "assertion" : type == _CRT_ERROR ? "error" : "warning",
             msg ? msg : "");
  if (ret) *ret = 0;
  return TRUE; // handled: no dialog
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
  log_stack(c->Esp, 16);
  return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install() {
  if (AddVectoredExceptionHandler(1, handler))
    log::info("Crash logger installed");
  else
    log::warn("Crash logger: AddVectoredExceptionHandler failed");
  _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, crt_report);
  std::set_terminate([] {
    log::error("std::terminate called (unhandled C++ exception?)");
    std::abort();
  });
  std::thread(watchdog).detach();
}

void beat_tick() {
  g_tick_thread = GetCurrentThreadId();
  g_last_beat = now_ms();
}

void beat_render() {
  g_render_thread = GetCurrentThreadId();
  g_last_beat = now_ms();
}

} // namespace ivc::crash
