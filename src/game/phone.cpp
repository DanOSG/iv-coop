#include "game/phone.h"

#include <windows.h>

#include <MinHook.h>

#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>

#include "core/log.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "game/script.h"

namespace ivc::phone {
namespace {

// Static VAs (GTAIV.exe 1.2.0.59). The script VM bounds-checks global pointers against
// [globals, globals + count*4) (scrThread::Run, e.g. 0x86DF50); CText::Get is the lookup
// behind GET_STRING_FROM_TEXT_FILE (handler 0xB8CB50 -> 0xB8FEC0), 382 callers.
constexpr std::uintptr_t kGlobalsPtr = 0x018B9B3C;
constexpr std::uintptr_t kGlobalsCount = 0x018B9B38;
constexpr std::uintptr_t kTextGet = 0x008C5BE0; // wchar_t* __thiscall CText::Get(const char* key)
constexpr int kPhoneGlobal = 91;
constexpr int kScreenOffset = 0, kDialledOffset = 128;

template <class T> T at_static(std::uintptr_t va) {
  return reinterpret_cast<T>(va - 0x400000 + reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}

std::uint8_t* global_ptr(int index) {
  auto* base = *at_static<std::uint8_t**>(kGlobalsPtr);
  const auto count = *at_static<std::int32_t*>(kGlobalsCount);
  if (!base || index >= count) return nullptr;
  return base + index * 4;
}

std::mutex g_mutex; // labels are read on whatever thread draws text
std::map<std::string, std::wstring> g_labels;
std::set<std::string> g_seen; // probe: keys looked up while the phone shows a network screen
std::int32_t g_last_screen = -1;
std::string g_last_dialled;
DWORD g_dump_at = 0;

using TextGetFn = const wchar_t*(__thiscall*)(void*, const char*);
TextGetFn g_text_get_orig = nullptr;

const wchar_t* __fastcall text_get_detour(void* self, void* /*edx*/, const char* key) {
  if (key && key[0]) {
    std::lock_guard lock(g_mutex);
    if (auto it = g_labels.find(key); it != g_labels.end()) return it->second.c_str();
    // Probe: remember the phone's network-menu labels the first time they're looked up.
    if (g_last_screen >= 1002 && g_last_screen < 1100 && g_last_screen != 1048 && g_seen.insert(key).second)
      log::info("Phone: screen {} shows text key {}", g_last_screen, key);
  }
  return g_text_get_orig(self, key);
}

} // namespace

bool install() {
  set_label("CP_OPT_NETWORK", "Multiplayer");
  void* target = at_static<void*>(kTextGet);
  if (MH_CreateHook(target, reinterpret_cast<void*>(&text_get_detour), reinterpret_cast<void**>(&g_text_get_orig)) !=
          MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    log::error("Phone: could not hook the text lookup");
    return false;
  }
  log::info("Phone: text lookup hooked");
  // Probe: the Multiplayer menu's match-type choice (0 player match, 1 ranked).
  native_hooks::on(0x47E03E87, "SET_CELLPHONE_RANKED", [](const native_hooks::Call& c) {
    log::info("Phone: {} picked match type {} (screen {})", c.script, c.arg<int>(0), g_last_screen);
  });
  return true;
}

std::int32_t screen() {
  const auto* g = global_ptr(kPhoneGlobal);
  return g ? *reinterpret_cast<const std::int32_t*>(g + kScreenOffset) : -1;
}

std::string dialled() {
  const auto* g = global_ptr(kPhoneGlobal);
  if (!g) return {};
  const char* s = reinterpret_cast<const char*>(g + kDialledOffset);
  return std::string(s, strnlen(s, 16));
}

// Probe: where the main menu keeps its items. Scans spcellphonemain's stack (statics
// first) for inline "CP_OPT_"/"CP_CHEATS" strings and logs each with the words around it.
void dump_menu_memory() {
  auto* t = static_cast<std::uint8_t*>(script::find_thread("spcellphonemain"));
  if (!t) {
    log::info("Phone probe: spcellphonemain not running");
    return;
  }
  const auto* stack = *reinterpret_cast<std::int32_t* const*>(t + 0x58);
  const auto size = *reinterpret_cast<const std::int32_t*>(t + 0x44);
  log::info("Phone probe: spcellphonemain thread {} stack {} size {}", static_cast<void*>(t),
            static_cast<const void*>(stack), size);
  if (!stack || size <= 0 || size > 0x100000) return;
  for (std::int32_t i = 0; i < size; ++i) {
    const char* s = reinterpret_cast<const char*>(stack + i);
    if (std::strncmp(s, "CP_", 3) != 0) continue;
    std::string ctx;
    for (int k = -6; k <= 5; ++k)
      if (i + k >= 0 && i + k < size) ctx += std::format("{}{:08X}", k == 0 ? " [" : " ", static_cast<std::uint32_t>(stack[i + k])) + (k == 0 ? "]" : "");
    log::info("Phone probe: slot {} '{}' |{}", i, std::string(s, strnlen(s, 16)), ctx);
  }
}

// --- main menu injection (spcellphonemain statics; layout of the 2019 scripts, verified
// against CE in memory): l_U1 is the list state (its _fU4, slot 2, is the item count;
// _fU60, slot 16, the id of the item being acted on), l_U226 the item array (slot 226 holds
// its capacity, element i at slot 227 + 18*i: +0 id, +4 32-byte text key, +68 enabled).
constexpr int kSlotCount = 2, kSlotArray = 226, kItemSlots = 18;
constexpr std::int32_t kMultiplayerId = 2000; // our own id: CE's menu handler ignores it
std::int32_t g_probe_slots[40] = {};

std::int32_t* statics() {
  auto* t = static_cast<std::uint8_t*>(script::find_thread("spcellphonemain"));
  return t ? *reinterpret_cast<std::int32_t**>(t + 0x58) : nullptr;
}

void inject_multiplayer(std::int32_t* st) {
  const std::int32_t count = st[kSlotCount], cap = st[kSlotArray];
  if (count <= 0 || count >= cap || cap > 64) return;
  for (std::int32_t i = 0; i < count; ++i)
    if (st[kSlotArray + 1 + i * kItemSlots] == kMultiplayerId) return; // already there
  std::int32_t* item = st + kSlotArray + 1 + count * kItemSlots;
  std::memset(item, 0, kItemSlots * 4);
  item[0] = kMultiplayerId;
  std::strncpy(reinterpret_cast<char*>(item + 1), "CP_OPT_NETWORK", 31);
  item[17] = 1; // _fU68: enabled
  st[kSlotCount] = count + 1;
  log::info("Phone: added Multiplayer to the main menu (item {})", count);
}

void probe_list_state(std::int32_t* st) {
  for (int i = 1; i < 40; ++i)
    if (st[i] != g_probe_slots[i]) {
      log::info("Phone probe: l_U slot {} {} -> {}", i, g_probe_slots[i], st[i]);
      g_probe_slots[i] = st[i];
    }
}

void tick() {
  if (screen() == 1011)
    if (std::int32_t* st = statics()) {
      inject_multiplayer(st);
      probe_list_state(st);
    }
  static bool checked = false;
  if (!checked) {
    checked = true;
    log::info("Phone probe: spcellphonenetwork exists: {}",
              natives::invoke<bool>(0x1D1B266B, "spcellphonenetwork")); // DOES_SCRIPT_EXIST
  }
  const std::int32_t s = screen();
  if (s == 1011 && g_last_screen != 1011) g_dump_at = GetTickCount() + 500; // after the list is built
  if (g_dump_at && GetTickCount() >= g_dump_at) {
    g_dump_at = 0;
    dump_menu_memory();
  }
  const std::string d = dialled();
  if (s != g_last_screen) {
    log::info("Phone: screen {} -> {}", g_last_screen, s);
    std::lock_guard lock(g_mutex);
    g_last_screen = s;
  }
  if (d != g_last_dialled) {
    log::info("Phone: dialled '{}'", d);
    g_last_dialled = d;
  }
}

void set_label(const std::string& key, const std::string& text) {
  std::lock_guard lock(g_mutex);
  g_labels[key] = std::wstring(text.begin(), text.end());
}

void clear_label(const std::string& key) {
  std::lock_guard lock(g_mutex);
  g_labels.erase(key);
}

} // namespace ivc::phone
