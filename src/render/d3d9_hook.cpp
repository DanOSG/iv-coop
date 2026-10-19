#include "render/d3d9_hook.h"

#include <d3d9.h>

#include <MinHook.h>

#include <atomic>

#include "core/iat.h"
#include "core/log.h"
#include "render/overlay.h"

namespace ivc::d3d9_hook {
namespace {

// IDirect3D9 / IDirect3DDevice9 vtable slots.
constexpr int kCreateDeviceIdx = 16;
constexpr int kResetIdx = 16;
constexpr int kPresentIdx = 17;
constexpr int kSwapChainPresentIdx = 3; // IDirect3DSwapChain9

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SwapChainPresent_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
                                                       const RGNDATA*, DWORD);

Direct3DCreate9_t o_Direct3DCreate9 = nullptr;
CreateDevice_t o_CreateDevice = nullptr;
Reset_t o_Reset = nullptr;
Present_t o_Present = nullptr;
SwapChainPresent_t o_SwapChainPresent = nullptr;

// Device returned by CreateDevice. Because GTAIV.exe runs under an apphelp compatibility shim
// (and FusionFix's d3d9.dll may wrap the device), this need not be the object that actually
// presents, so it is only used for diagnostics.
std::atomic<IDirect3DDevice9*> g_created{nullptr};
// The device that actually reaches the real Present: the one the overlay draws on.
std::atomic<IDirect3DDevice9*> g_presenting{nullptr};

void* vfunc(void* obj, int idx) { return (*static_cast<void***>(obj))[idx]; }

// Set while inside the device Present hook, so a device Present that internally routes through
// the swap chain's Present doesn't draw the overlay twice.
thread_local bool t_in_present = false;

// The hooks are on the shared d3d9 implementation; lock onto the first device that presents
// after the game created its device, and ignore any others.
void present_common(IDirect3DDevice9* dev, const char* via) {
  IDirect3DDevice9* presenting = g_presenting.load(std::memory_order_relaxed);
  if (!presenting && g_created.load(std::memory_order_relaxed)) {
    g_presenting = presenting = dev;
    log::info("First Present via {} on device {} (CreateDevice returned {})", via, static_cast<void*>(dev),
              static_cast<void*>(g_created.load()));
  }
  if (dev == presenting) overlay::on_present(dev);
}

HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd,
                                     const RGNDATA* dirty) {
  present_common(dev, "device");
  t_in_present = true;
  const HRESULT hr = o_Present(dev, src, dst, wnd, dirty);
  t_in_present = false;
  return hr;
}

HRESULT STDMETHODCALLTYPE hk_SwapChainPresent(IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND wnd,
                                              const RGNDATA* dirty, DWORD flags) {
  if (!t_in_present) {
    IDirect3DDevice9* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(&dev)) && dev) {
      present_common(dev, "swapchain");
      dev->Release();
    }
  }
  return o_SwapChainPresent(sc, src, dst, wnd, dirty, flags);
}

HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
  const bool ours = dev == g_presenting.load(std::memory_order_relaxed);
  if (ours) overlay::on_before_reset();
  const HRESULT hr = o_Reset(dev, pp);
  if (ours) {
    log::info("Device reset: hr=0x{:08X} {}x{}", static_cast<unsigned>(hr), pp->BackBufferWidth, pp->BackBufferHeight);
    if (SUCCEEDED(hr)) overlay::on_after_reset();
  }
  return hr;
}

bool detour(void* target, void* hook, void** orig, const char* name) {
  MH_STATUS st = MH_CreateHook(target, hook, orig);
  if (st == MH_OK) st = MH_EnableHook(target);
  if (st != MH_OK) {
    log::error("Hooking {} at {} failed: {}", name, target, MH_StatusToString(st));
    return false;
  }
  log::info("Hooked {} at {}", name, target);
  return true;
}

HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                          D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
  const HRESULT hr = o_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
  log::info("CreateDevice: hr=0x{:08X} {}x{} windowed={} focus={}", static_cast<unsigned>(hr), pp->BackBufferWidth,
            pp->BackBufferHeight, pp->Windowed, static_cast<void*>(focus));
  if (FAILED(hr) || !out || !*out) return hr;

  IDirect3DDevice9* dev = *out;
  if (g_created.exchange(dev) == nullptr) {
    // First device: install the per-device detours once.
    detour(vfunc(dev, kPresentIdx), &hk_Present, reinterpret_cast<void**>(&o_Present), "IDirect3DDevice9::Present");
    detour(vfunc(dev, kResetIdx), &hk_Reset, reinterpret_cast<void**>(&o_Reset), "IDirect3DDevice9::Reset");
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
      detour(vfunc(sc, kSwapChainPresentIdx), &hk_SwapChainPresent, reinterpret_cast<void**>(&o_SwapChainPresent),
             "IDirect3DSwapChain9::Present");
      sc->Release();
    } else {
      log::warn("GetSwapChain(0) failed; swap chain Present not hooked");
    }
  } else {
    log::warn("Game created a second D3D9 device; overlay stays on {}", static_cast<void*>(g_presenting.load()));
  }
  overlay::set_window(pp->hDeviceWindow ? pp->hDeviceWindow : focus);
  return hr;
}

IDirect3D9* WINAPI hk_Direct3DCreate9(UINT sdk) {
  IDirect3D9* d3d = o_Direct3DCreate9(sdk);
  log::info("Direct3DCreate9(sdk={}) -> {}", sdk, static_cast<void*>(d3d));
  if (d3d && !o_CreateDevice)
    detour(vfunc(d3d, kCreateDeviceIdx), &hk_CreateDevice, reinterpret_cast<void**>(&o_CreateDevice),
           "IDirect3D9::CreateDevice");
  return d3d;
}

} // namespace

bool install() {
  void* prev = iat::hook(GetModuleHandleW(nullptr), "d3d9.dll", "Direct3DCreate9",
                         reinterpret_cast<void*>(&hk_Direct3DCreate9));
  if (!prev) {
    log::error("Direct3DCreate9 import not found in host exe; overlay disabled");
    return false;
  }
  o_Direct3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(prev);
  log::info("Patched Direct3DCreate9 import (was {})", prev);
  return true;
}

} // namespace ivc::d3d9_hook
