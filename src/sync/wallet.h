#pragma once
// A joiner's money (ARCHITECTURE, "Money").
//
// While joined, a player spends and picks up from their wallet for that host's campaign, not their
// single-player money. On join the single-player amount is put aside and the wallet loaded (a first
// visit starts with kStartingCash); while joined the wallet is saved every 10 s; on leave it is saved
// and the single-player money put back. Wallets live in plugins/iv-coop/wallets.ini, one per host
// (by name). Mirrored mission payouts land in the wallet like any other money.
// Limit: if the game crashes while joined, the single-player amount isn't put back automatically; it's kept
// in wallets.ini [SinglePlayer] Money and a warning is logged on the next start.

#include <filesystem>
#include <string>

namespace ivc::wallet {

constexpr int kStartingCash = 1000;

void configure(const std::filesystem::path& plugin_dir);
// Game thread, every frame. `host_name` empty until the host's name is known.
void tick(bool joined, const std::string& host_name);

} // namespace ivc::wallet
