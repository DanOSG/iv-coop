#include "sync/wallet.h"

#include <windows.h>

#include "core/log.h"
#include "game/natives.h"

namespace ivc::wallet {
namespace {
namespace n = natives;
using n::invoke;

constexpr std::uint32_t kStoreScore = 0x1E203014;
constexpr DWORD kSaveMs = 10000;

std::wstring g_ini;
bool g_active = false;    // our money is the wallet
std::string g_host;       // whose campaign
int g_single_player = 0;  // put aside on join
DWORD g_last_save = 0;

int money() {
  int v = 0;
  invoke(kStoreScore, n::GetPlayerId(), &v);
  return v;
}

void set_money(int v) { n::AddScore(n::GetPlayerId(), v - money()); }

std::wstring key_for(const std::string& host) {
  std::wstring k;
  for (char c : host) k += (c == '=' || c == '[' || c == ']' || c == ';') ? L'_' : static_cast<wchar_t>(static_cast<unsigned char>(c));
  return k.empty() ? L"unknown" : k;
}

int read_int(const wchar_t* section, const std::wstring& key, int def) {
  return static_cast<int>(GetPrivateProfileIntW(section, key.c_str(), def, g_ini.c_str()));
}

void write_int(const wchar_t* section, const std::wstring& key, int v) {
  WritePrivateProfileStringW(section, key.c_str(), std::to_wstring(v).c_str(), g_ini.c_str());
}

void save_wallet() { write_int(L"Wallets", key_for(g_host), money()); }

} // namespace

void configure(const std::filesystem::path& plugin_dir) {
  std::error_code ec;
  std::filesystem::create_directories(plugin_dir / "iv-coop", ec);
  g_ini = (plugin_dir / "iv-coop" / "wallets.ini").wstring();
  const int pending = read_int(L"SinglePlayer", L"Pending", 0);
  if (pending)
    log::warn("Wallet: a co-op session ended without restoring single-player money (${}); it's in "
              "plugins\\iv-coop\\wallets.ini [SinglePlayer] Money",
              read_int(L"SinglePlayer", L"Money", 0));
}

void tick(bool joined, const std::string& host_name) {
  if (joined && !g_active) {
    if (host_name.empty()) return; // wait until we know whose campaign this is
    g_host = host_name;
    g_single_player = money();
    write_int(L"SinglePlayer", L"Money", g_single_player);
    write_int(L"SinglePlayer", L"Pending", 1);
    const int wallet = read_int(L"Wallets", key_for(g_host), kStartingCash);
    set_money(wallet);
    g_active = true, g_last_save = GetTickCount();
    log::info("Wallet: ${} for {}'s campaign (single-player ${} put aside)", wallet, g_host, g_single_player);
    return;
  }
  if (!g_active) return;
  if (!joined) {
    save_wallet();
    const int wallet = money();
    set_money(g_single_player);
    write_int(L"SinglePlayer", L"Pending", 0);
    g_active = false;
    log::info("Wallet: left with ${} (saved); single-player ${} back", wallet, g_single_player);
    return;
  }
  if (GetTickCount() - g_last_save >= kSaveMs) {
    g_last_save = GetTickCount();
    save_wallet();
  }
}

} // namespace ivc::wallet
