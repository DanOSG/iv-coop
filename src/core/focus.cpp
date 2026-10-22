#include "core/focus.h"

#include <windows.h>

#include <atomic>

#include "core/log.h"

namespace ivc::focus {
namespace {

std::atomic<bool> g_on{false};
HWND g_wnd = nullptr;
WNDPROC g_orig = nullptr;

LRESULT CALLBACK proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (g_on) {
    if (msg == WM_ACTIVATEAPP && !wp) return 0;
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) return 0;
    if (msg == WM_KILLFOCUS) return 0;
  }
  return CallWindowProcW(g_orig, wnd, msg, wp, lp);
}

BOOL CALLBACK find(HWND wnd, LPARAM out) {
  DWORD pid = 0;
  GetWindowThreadProcessId(wnd, &pid);
  if (pid == GetCurrentProcessId() && IsWindowVisible(wnd) && !GetWindow(wnd, GW_OWNER)) {
    *reinterpret_cast<HWND*>(out) = wnd;
    return FALSE;
  }
  return TRUE;
}

} // namespace

void keep_running(bool on) {
  if (on && !g_orig) {
    EnumWindows(find, reinterpret_cast<LPARAM>(&g_wnd));
    if (!g_wnd) {
      log::warn("Focus: game window not found; it will still pause in the background");
      return;
    }
    g_orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&proc)));
  }
  if (g_on.exchange(on) != on)
    log::info("Focus: {}", on ? "game keeps running in the background" : "normal (pauses in the background)");
}

bool keeping_running() { return g_on; }

} // namespace ivc::focus
