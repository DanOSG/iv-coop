#include "game/pattern.h"

#include <windows.h>

#include <cstring>

namespace ivc::pattern {
namespace {

struct Compiled {
  std::vector<std::uint8_t> bytes;
  std::vector<std::uint8_t> mask; // 1 = must match
};

Compiled compile(std::string_view p) {
  Compiled c;
  size_t i = 0;
  while (i < p.size()) {
    if (p[i] == ' ') { ++i; continue; }
    if (p[i] == '?') {
      c.bytes.push_back(0);
      c.mask.push_back(false);
      while (i < p.size() && p[i] == '?') ++i;
      continue;
    }
    auto hex = [](char ch) -> int {
      if (ch >= '0' && ch <= '9') return ch - '0';
      if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
      if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
      return 0;
    };
    c.bytes.push_back(static_cast<std::uint8_t>(hex(p[i]) << 4 | hex(p[i + 1])));
    c.mask.push_back(true);
    i += 2;
  }
  return c;
}

std::pair<const std::uint8_t*, size_t> text_section() {
  auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    if (std::memcmp(sec->Name, ".text", 5) == 0) return {base + sec->VirtualAddress, sec->Misc.VirtualSize};
  return {nullptr, 0};
}

} // namespace

std::vector<std::uintptr_t> find_all(std::string_view pattern) {
  const Compiled c = compile(pattern);
  const auto [text, size] = text_section();
  std::vector<std::uintptr_t> hits;
  if (!text || c.bytes.empty() || size < c.bytes.size()) return hits;

  // Jump between occurrences of the first fixed byte with memchr (vectorised in the CRT),
  // then verify the rest. Patterns always start with a fixed byte in practice.
  const size_t n = c.bytes.size();
  size_t anchor = 0;
  while (anchor < n && !c.mask[anchor]) ++anchor;
  if (anchor == n) return hits;

  const std::uint8_t* const end = text + size - (n - anchor) + 1;
  const std::uint8_t* p = text + anchor;
  while (p < end) {
    p = static_cast<const std::uint8_t*>(std::memchr(p, c.bytes[anchor], static_cast<size_t>(end - p)));
    if (!p) break;
    const std::uint8_t* start = p - anchor;
    size_t j = 0;
    while (j < n && (!c.mask[j] || start[j] == c.bytes[j])) ++j;
    if (j == n) hits.push_back(reinterpret_cast<std::uintptr_t>(start));
    ++p;
  }
  return hits;
}

} // namespace ivc::pattern
