#include "net/upnp.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <natupnp.h>
#include <oleauto.h>

#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <thread>

#include "core/log.h"

namespace ivc::upnp {
namespace {

struct Job {
  bool open;
  std::uint16_t port;
};

// Never destroyed: the worker may still be inside a slow COM call at process exit.
struct State {
  std::mutex m;
  std::condition_variable cv;
  std::deque<Job> jobs;
  bool worker_started = false;
  bool busy = false; // the worker is inside a job
  std::optional<std::string> report; // not yet read by poll()
  std::uint16_t mapped_port = 0;
};
State& S() {
  static State* s = new State;
  return *s;
}

// The local address the OS would use to reach the internet (a connected UDP socket sends
// nothing, it only picks the route), which is what the router must forward to.
std::string local_address() {
  std::string out;
  SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s == INVALID_SOCKET) return out;
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(9);
  inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
  if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
    sockaddr_in me{};
    int len = sizeof(me);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0 && me.sin_addr.s_addr != 0) {
      char buf[INET_ADDRSTRLEN] = {};
      inet_ntop(AF_INET, &me.sin_addr, buf, sizeof(buf));
      out = buf;
    }
  }
  closesocket(s);
  return out;
}

void report(std::string text) {
  log::info("UPnP: {}", text);
  State& s = S();
  std::lock_guard lock(s.m);
  s.report = std::move(text);
}

void do_open(std::uint16_t port) {
  IUPnPNAT* nat = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nat));
  if (FAILED(hr) || !nat) {
    report(std::format("Couldn't open the port: no UPnP support on this PC; friends join via Steam"));
    return;
  }
  IStaticPortMappingCollection* maps = nullptr;
  hr = nat->get_StaticPortMappingCollection(&maps);
  if (FAILED(hr) || !maps) { // no UPnP router answered (disabled, or double NAT)
    nat->Release();
    report(std::format("Couldn't open the port: your router doesn't offer UPnP; friends join via Steam"));
    return;
  }
  const std::string local = local_address();
  if (local.empty()) {
    maps->Release(), nat->Release();
    report("Couldn't open the port: can't tell this PC's network address; friends join via Steam");
    return;
  }
  BSTR stale = SysAllocString(L"UDP");
  maps->Remove(port, stale); // replace a stale mapping left by a crash
  SysFreeString(stale);
  std::wstring wlocal(local.begin(), local.end());
  BSTR proto = SysAllocString(L"UDP"), client = SysAllocString(wlocal.c_str()), desc = SysAllocString(L"iv-coop");
  IStaticPortMapping* mapping = nullptr;
  hr = maps->Add(port, proto, port, client, VARIANT_TRUE, desc, &mapping);
  SysFreeString(proto), SysFreeString(client), SysFreeString(desc);
  if (FAILED(hr) || !mapping) {
    maps->Release(), nat->Release();
    report(std::format("Couldn't open the port: the router refused (0x{:08X}); friends join via Steam", static_cast<unsigned>(hr)));
    return;
  }
  std::string external;
  BSTR ext = nullptr;
  if (SUCCEEDED(mapping->get_ExternalIPAddress(&ext)) && ext) {
    for (const wchar_t* p = ext; *p; ++p) external += static_cast<char>(*p);
    SysFreeString(ext);
  }
  mapping->Release(), maps->Release(), nat->Release();
  {
    std::lock_guard lock(S().m);
    S().mapped_port = port;
  }
  report(external.empty() ? std::format("Port {} opened on your router", port)
                          : std::format("Port {} opened on your router (public address {})", port, external));
}

void do_close(std::uint16_t port) {
  IUPnPNAT* nat = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nat))) || !nat) return;
  IStaticPortMappingCollection* maps = nullptr;
  if (SUCCEEDED(nat->get_StaticPortMappingCollection(&maps)) && maps) {
    BSTR proto = SysAllocString(L"UDP");
    const HRESULT hr = maps->Remove(port, proto);
    SysFreeString(proto);
    log::info("UPnP: removed the mapping for UDP {} (0x{:08X})", port, static_cast<unsigned>(hr));
    maps->Release();
  }
  nat->Release();
  std::lock_guard lock(S().m);
  if (S().mapped_port == port) S().mapped_port = 0;
}

void worker() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  State& s = S();
  for (;;) {
    Job job;
    {
      std::unique_lock lock(s.m);
      s.cv.wait(lock, [&] { return !s.jobs.empty(); });
      job = s.jobs.front();
      s.jobs.pop_front();
      s.busy = true;
    }
    // A close queued behind an open for the same port cancels it: skip straight to cleanup.
    if (job.open) {
      bool cancelled = false;
      {
        std::lock_guard lock(s.m);
        cancelled = !s.jobs.empty() && !s.jobs.front().open;
      }
      if (!cancelled) do_open(job.port);
    } else {
      std::uint16_t port;
      {
        std::lock_guard lock(s.m);
        port = s.mapped_port;
      }
      if (port) do_close(port);
    }
    std::lock_guard lock(s.m);
    s.busy = false;
  }
}

void enqueue(Job j) {
  State& s = S();
  std::lock_guard lock(s.m);
  if (!s.worker_started) {
    s.worker_started = true;
    std::thread(worker).detach();
  }
  s.jobs.push_back(j);
  s.cv.notify_one();
}

} // namespace

void open(std::uint16_t port) {
  {
    std::lock_guard lock(S().m);
    S().report.reset();
  }
  enqueue({true, port});
}

void close() {
  {
    std::lock_guard lock(S().m);
    S().report.reset();
    if (!S().mapped_port && S().jobs.empty() && !S().busy) return; // nothing mapped or pending
  }
  enqueue({false, 0});
}

std::optional<std::string> poll() {
  std::lock_guard lock(S().m);
  auto r = std::move(S().report);
  S().report.reset();
  return r;
}

bool mapped() {
  std::lock_guard lock(S().m);
  return S().mapped_port != 0;
}

} // namespace ivc::upnp
