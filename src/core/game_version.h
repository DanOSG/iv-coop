#pragma once
#include <string>

namespace ivc {

struct GameVersion {
  unsigned major = 0, minor = 0, build = 0, revision = 0;
  std::string str() const;
  bool operator==(const GameVersion&) const = default;
};

// File version of the host executable (GTAIV.exe / EFLC.exe).
GameVersion query_game_version();

// The only executable build we develop and test against.
inline constexpr GameVersion kSupportedVersion{1, 2, 0, 59};

} // namespace ivc
