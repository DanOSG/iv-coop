#include "game/phone.h"

#include <windows.h>

#include <MinHook.h>

#include <cctype>
#include <cstring>
#include <format>
#include <map>
#include <mutex>

#include "core/log.h"
#include "game/natives.h"
#include "game/script.h"

namespace ivc::phone {
namespace {
namespace n = natives;

// Static VAs (GTAIV.exe 1.2.0.59). The script VM bounds-checks global pointers against
// [globals, globals + count*4) (scrThread::Run, e.g. 0x86DF50); CText::Get is the lookup
// behind GET_STRING_FROM_TEXT_FILE (handler 0xB8CB50 -> 0xB8FEC0), 382 callers.
constexpr std::uintptr_t kGlobalsPtr = 0x018B9B3C;
constexpr std::uintptr_t kGlobalsCount = 0x018B9B38;
constexpr std::uintptr_t kTextGet = 0x008C5BE0; // wchar_t* __thiscall CText::Get(const char* key)
constexpr int kPhoneGlobal = 91;
constexpr int kScreenOffset = 0, kDialledOffset = 128;

// Phone screens (g_U91._fU0).
constexpr std::int32_t kScreenHome = 1001, kScreenMainMenu = 1011, kScreenCalling = 1005;

// spcellphonemain statics (2019 script layout, verified against CE in memory). The thread
// stack is at +0x58 (statics first). l_U1 is the list state: _fU4 (slot 2) the item count;
// l_U226 the item array: capacity at slot 226, element i at 227 + 18*i (+0 id, +4 32-byte
// text key, +68 enabled). l_U958 is the "screen changed" flag the script's own switch
// helper (sub_3706) sets alongside g_U91._fU0.
constexpr int kSlotCount = 2, kSlotArray = 226, kItemSlots = 18, kSlotScreenChanged = 958;
constexpr std::int32_t kIdMultiplayer = 2000, kIdHost = 2001, kIdJoin = 2002, kIdLeave = 2003;
constexpr std::size_t kPasswordDigits = 6;

template <class T> T at_static(std::uintptr_t va) {
  return reinterpret_cast<T>(va - 0x400000 + reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
}

std::uint8_t* global_ptr(int index) {
  auto* base = *at_static<std::uint8_t**>(kGlobalsPtr);
  const auto count = *at_static<std::int32_t*>(kGlobalsCount);
  if (!base || index >= count) return nullptr;
  return base + index * 4;
}

std::int32_t* main_statics() {
  auto* t = static_cast<std::uint8_t*>(script::find_thread("spcellphonemain"));
  return t ? *reinterpret_cast<std::int32_t**>(t + 0x58) : nullptr;
}

std::mutex g_mutex; // labels are read on whatever thread draws text
std::map<std::string, std::wstring> g_labels;

using TextGetFn = const wchar_t*(__thiscall*)(void*, const char*);
TextGetFn g_text_get_orig = nullptr;

const wchar_t* __fastcall text_get_detour(void* self, void* /*edx*/, const char* key) {
  // Only our keys (IVC_*, and the removed CP_OPT_NETWORK): the game calls this constantly.
  if (key && (std::strncmp(key, "IVC_", 4) == 0 || std::strcmp(key, "CP_OPT_NETWORK") == 0)) {
    std::lock_guard lock(g_mutex);
    if (auto it = g_labels.find(key); it != g_labels.end()) return it->second.c_str();
  }
  return g_text_get_orig(self, key);
}

bool g_in_session = false;
bool g_coop_menu = false;                 // the main menu currently shows our items
std::optional<Action> g_awaiting;         // Host/Join chosen; waiting for the password
std::optional<Request> g_request;         // ready for poll()

void switch_screen(std::int32_t to) {
  auto* g = global_ptr(kPhoneGlobal);
  std::int32_t* st = main_statics();
  if (!g || !st) return;
  *reinterpret_cast<std::int32_t*>(g + kScreenOffset) = to;
  st[kSlotScreenChanged] = 1;
}

void write_item(std::int32_t* st, std::int32_t index, std::int32_t id, const char* key) {
  std::int32_t* item = st + kSlotArray + 1 + index * kItemSlots;
  std::memset(item, 0, kItemSlots * 4);
  item[0] = id;
  std::strncpy(reinterpret_cast<char*>(item + 1), key, 31);
  item[17] = 1; // _fU68: enabled
}

bool has_item(const std::int32_t* st, std::int32_t id) {
  for (std::int32_t i = 0; i < st[kSlotCount]; ++i)
    if (st[kSlotArray + 1 + i * kItemSlots] == id) return true;
  return false;
}

// Main menu (1011): the normal list plus "Multiplayer", or our co-op items.
void fix_main_menu(std::int32_t* st) {
  const std::int32_t count = st[kSlotCount], cap = st[kSlotArray];
  if (count <= 0 || cap <= 0 || cap > 64) return;
  if (g_coop_menu) {
    const std::int32_t first = g_in_session ? kIdLeave : kIdHost;
    if (st[kSlotArray + 1] == first && count == (g_in_session ? 1 : 2)) return;
    if (g_in_session) {
      write_item(st, 0, kIdLeave, "IVC_LEAVE");
      st[kSlotCount] = 1;
    } else {
      write_item(st, 0, kIdHost, "IVC_HOST");
      write_item(st, 1, kIdJoin, "IVC_JOIN");
      st[kSlotCount] = 2;
    }
    return;
  }
  if (count < cap && !has_item(st, kIdMultiplayer)) {
    write_item(st, count, kIdMultiplayer, "CP_OPT_NETWORK");
    st[kSlotCount] = count + 1;
  }
}

void show_help(const std::string& text, bool forever) {
  set_label("IVC_MSG", text);
  n::ClearHelp();
  if (forever)
    n::PrintHelpForever("IVC_MSG");
  else
    n::PrintHelp("IVC_MSG");
}

} // namespace

bool install() {
  set_label("CP_OPT_NETWORK", "Multiplayer");
  set_label("IVC_HOST", "Host Co-op");
  set_label("IVC_JOIN", "Join Co-op");
  set_label("IVC_LEAVE", "Leave Co-op");
  void* target = at_static<void*>(kTextGet);
  if (MH_CreateHook(target, reinterpret_cast<void*>(&text_get_detour), reinterpret_cast<void**>(&g_text_get_orig)) !=
          MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    log::error("Phone: could not hook the text lookup");
    return false;
  }
  log::info("Phone: text lookup hooked");
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

void tick() {
  const std::int32_t s = screen();
  switch (s) {
    case kScreenMainMenu:
      if (std::int32_t* st = main_statics()) fix_main_menu(st);
      break;
    case kIdMultiplayer: // "Multiplayer" picked: show our items in the same list
      g_coop_menu = true;
      switch_screen(kScreenMainMenu);
      break;
    case kIdHost:
    case kIdJoin:
      g_coop_menu = false;
      g_awaiting = s == kIdHost ? Action::Host : Action::Join;
      switch_screen(kScreenHome);
      show_help(std::format("Type a {}-digit co-op password, then press Call.", kPasswordDigits), true);
      log::info("Phone: {} chosen; waiting for the password", s == kIdHost ? "Host" : "Join");
      break;
    case kIdLeave:
      g_coop_menu = false;
      g_request = Request{Action::Leave, {}};
      switch_screen(kScreenHome);
      break;
    case kScreenCalling:
      if (g_awaiting) {
        std::string digits;
        for (char ch : dialled())
          if (std::isdigit(static_cast<unsigned char>(ch))) digits += ch;
        n::ClearHelp();
        if (digits.size() >= kPasswordDigits) {
          digits.resize(kPasswordDigits);
          g_request = Request{*g_awaiting, digits};
          log::info("Phone: {} with password {}", *g_awaiting == Action::Host ? "host" : "join", digits);
        } else {
          show_help(std::format("The password needs {} digits.", kPasswordDigits), false);
        }
        g_awaiting.reset();
      }
      break;
    default:
      // Left the main menu (back/close): the next visit shows the normal list again.
      if (s != kScreenMainMenu) g_coop_menu = false;
      // Closed the phone without calling: forget the half-made choice.
      if (s == 1000 && g_awaiting) {
        g_awaiting.reset();
        n::ClearHelp();
      }
  }
}

std::optional<Request> poll() { return std::exchange(g_request, std::nullopt); }

void set_in_session(bool in) { g_in_session = in; }

void notify(const std::string& text) { show_help(text, false); }

void set_label(const std::string& key, const std::string& text) {
  std::lock_guard lock(g_mutex);
  g_labels[key] = std::wstring(text.begin(), text.end());
}

} // namespace ivc::phone
