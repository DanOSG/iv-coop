#include "core/iat.h"

#include <cstring>

namespace ivc::iat {

void* hook(HMODULE module, const char* dll, const char* func, void* replacement) {
  auto* base = reinterpret_cast<BYTE*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return nullptr;

  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
    if (!imp->OriginalFirstThunk) continue; // no name table: can't match by name

    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
    auto* addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++addrs) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), func) != 0) continue;

      void** slot = reinterpret_cast<void**>(&addrs->u1.Function);
      DWORD old;
      VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
      void* prev = *slot;
      *slot = replacement;
      VirtualProtect(slot, sizeof(void*), old, &old);
      return prev;
    }
  }
  return nullptr;
}

} // namespace ivc::iat
