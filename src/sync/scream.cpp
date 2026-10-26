#include "sync/scream.h"

#include <windows.h>

#include "core/input.h"
#include "core/log.h"
#include "net/bytes.h"

namespace ivc::scream {
namespace {
namespace n = natives;

// HIGH_FALL_SCREAM, ON_FIRE_SCREAM, PANIC_SCREAM (ped).
constexpr std::uint32_t kScreams[] = {0x478976DB, 0x6BE062DF, 0x4F8B4507};
constexpr DWORD kCooldownMs = 1500;

int g_key = 'J';
bool g_was_down = true;
DWORD g_last = 0;

void play(n::Ped ped, std::uint8_t kind) {
  if (ped && n::DoesCharExist(ped) && kind < std::size(kScreams)) n::invoke(kScreams[kind], ped);
}

} // namespace

void configure(const std::filesystem::path& ini) {
  g_key = static_cast<int>(GetPrivateProfileIntW(L"Fun", L"ScreamKey", 'J', ini.wstring().c_str()));
  WritePrivateProfileStringW(L"Fun", L"ScreamKey", std::to_wstring(g_key).c_str(), ini.wstring().c_str());
}

void tick(bool in_session, net::Session& net) {
  if (!g_key || !input::pressed(g_key, g_was_down)) return;
  const DWORD now = GetTickCount();
  if (now - g_last < kCooldownMs) return;
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  if (!me || !n::DoesCharExist(me) || n::IsCharDead(me)) return;
  g_last = now;
  const auto kind = static_cast<std::uint8_t>(now % std::size(kScreams));
  play(me, kind);
  if (in_session) {
    net::Writer w;
    w.put(kMsg).put(std::uint8_t{0}).put(kind);
    net.send_game(std::move(w.data()), true);
  }
}

void on_message(const net::GameMessage& m, n::Ped ped) {
  net::Reader r(m.data);
  std::uint8_t type, sender, kind;
  if (r.get(type) && r.get(sender) && r.get(kind)) play(ped, kind);
}

} // namespace ivc::scream
