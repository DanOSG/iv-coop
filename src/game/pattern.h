#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace ivc::pattern {

// Finds every match of an IDA-style pattern ("8B 35 ? ? ? ? 85 F6") in the host exe's
// .text section, in address order.
std::vector<std::uintptr_t> find_all(std::string_view pattern);

// Reads a pointer-sized immediate at `addr` (e.g. the operand of `mov esi, [imm32]`).
template <class T>
T read(std::uintptr_t addr) {
  return *reinterpret_cast<T*>(addr);
}

} // namespace ivc::pattern
