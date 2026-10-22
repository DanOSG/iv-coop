#include "game/phone.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
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
// Get hashes the key and calls GetByHash; drawing code (e.g. the phone's menu) can call
// GetByHash or Exists directly, so all three are hooked for our keys.
constexpr std::uintptr_t kTextGetByHash = 0x008C5AE0; // wchar_t* __thiscall (u32 hash, ?)
constexpr std::uintptr_t kTextExists = 0x008C5A40;    // bool __thiscall (const char* key)
constexpr std::uintptr_t kKeyHash = 0x0040BA60;       // u32 __cdecl (const char* key, u32 seed): the game's own
constexpr int kPhoneGlobal = 91;
constexpr int kScreenOffset = 0, kDialledOffset = 128;

// Phone screens (g_U91._fU0).
// 1001 is the phone closing (then 1000 idle); 1003 is the keypad (entering it clears the
// number being dialled); Call there goes to 1005 (calling).
constexpr std::int32_t kScreenClose = 1001, kScreenKeypad = 1003, kScreenMainMenu = 1011, kScreenCalling = 1005;

// spcellphonemain statics (2019 script layout, verified against CE in memory). The thread
// stack is at +0x58 (statics first). l_U1 is the list state: _fU4 (slot 2) the item count;
// l_U226 the item array: capacity at slot 226, element i at 227 + 18*i (+0 id, +4 32-byte
// text key, +68 enabled). l_U958 is the "screen changed" flag the script's own switch
// helper (sub_3706) sets alongside g_U91._fU0.
constexpr int kSlotCount = 2, kSlotArray = 226, kItemSlots = 18, kSlotScreenChanged = 958;
// l_U1 display state (sub_10177 fills it from the items when the screen is built):
// _fU0 menu type (slot 1), _fU8 remembered index (3), _fU12 its flag (4), _fU16[] visible
// window (size at 5, rows 6..), _fU44 cursor row (12), _fU48 visible count (13), _fU52
// scrolls (14), _fU56 scrollbar size (15, float), _fU60 selected id (16). l_U39 is the row
// array: size at slot 39, row i at 40 + 31*i (+0 valid, +4 id, +8 32-byte key, +40/44/48
// item +36/40/44, +52 16-byte key2, +68/72 item +64/68).
constexpr int kSlotMenuType = 1, kSlotMemory = 3, kSlotMemoryFlag = 4, kSlotWindow = 6, kSlotCursor = 12,
              kSlotVisible = 13, kSlotScrolls = 14, kSlotScrollSize = 15, kSlotSelected = 16, kSlotRows = 39,
              kRowSlots = 31, kMaxRows = 6;
constexpr int kG91Rows = 500, kG91Style = 40, kG91Speed = 492; // g_U91 byte offsets used by sub_10177
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
using TextGetByHashFn = const wchar_t*(__thiscall*)(void*, std::uint32_t, void*);
TextGetByHashFn g_get_by_hash_orig = nullptr;
using TextExistsFn = bool(__thiscall*)(void*, const char*);
TextExistsFn g_exists_orig = nullptr;
std::map<std::uint32_t, std::wstring> g_hashed; // our labels by the game's key hash

bool ours(const char* key) {
  return key && (std::strncmp(key, "IVC_", 4) == 0 || std::strcmp(key, "CP_OPT_NETWORK") == 0);
}

const wchar_t* __fastcall get_by_hash_detour(void* self, void* /*edx*/, std::uint32_t hash, void* p2) {
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_hashed.find(hash); it != g_hashed.end()) return it->second.c_str();
  }
  return g_get_by_hash_orig(self, hash, p2);
}

bool __fastcall exists_detour(void* self, void* /*edx*/, const char* key) {
  if (ours(key)) {
    std::lock_guard lock(g_mutex);
    if (g_labels.contains(key)) return true;
  }
  return g_exists_orig(self, key);
}

// Hashes are computed with the game's own function, which is only usable once the game
// runs (its code is decrypted at startup), so labels are hashed from tick().
bool g_hash_dirty = true;
void rehash_labels() {
  const auto hash = at_static<std::uint32_t(__cdecl*)(const char*, std::uint32_t)>(kKeyHash);
  std::lock_guard lock(g_mutex);
  for (const auto& [key, text] : g_labels) g_hashed.try_emplace(hash(key.c_str(), 0), text); // never replaced
  g_hash_dirty = false;
}

bool hook(std::uintptr_t va, void* detour, void** orig, const char* what) {
  void* target = at_static<void*>(va);
  if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
    log::error("Phone: could not hook {}", what);
    return false;
  }
  return true;
}

const wchar_t* __fastcall text_get_detour(void* self, void* /*edx*/, const char* key) {
  // Only our keys (IVC_*, and the removed CP_OPT_NETWORK): the game calls this constantly.
  if (ours(key)) {
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

// Redo sub_10177's bookkeeping for the current items (cursor on the first item), after we
// changed the list behind the script's back. Without it the window/rows describe the old
// list and scrolling walks off the end (the phone script crashed and restarted).
void rebuild_display(std::int32_t* st) {
  std::uint8_t* g = global_ptr(kPhoneGlobal);
  if (!g) return;
  const std::int32_t count = st[kSlotCount];
  if (count <= 0) return;
  auto g_int = [g](int off) -> std::int32_t& { return *reinterpret_cast<std::int32_t*>(g + off); };
  g_int(kG91Speed) = std::clamp<std::int32_t>(1000 / count, 100, 500);
  std::int32_t rows = g_int(kG91Rows) ? 6 : 4;
  if (st[kSlotMenuType] == 2) rows /= 2;
  st[kSlotMemory] = 0;
  st[kSlotMemoryFlag] = 0;
  for (std::int32_t i = 0; i < rows; ++i) st[kSlotWindow + i] = i;
  st[kSlotCursor] = 0;
  if (count < rows) {
    st[kSlotVisible] = count;
    st[kSlotScrolls] = 0;
    rows = count;
  } else {
    st[kSlotVisible] = rows;
    st[kSlotScrolls] = 1;
    const float size = (g_int(kG91Style) == 2 ? 0.72f : 0.75f) / static_cast<float>(count);
    std::memcpy(&st[kSlotScrollSize], &size, 4);
  }
  for (int r = 0; r < kMaxRows; ++r) st[kSlotRows + 1 + r * kRowSlots] = 0; // sub_10952: row invalid
  for (int r = 0; r < rows; ++r) {
    const std::int32_t* item = st + kSlotArray + 1 + st[kSlotWindow + r] * kItemSlots;
    std::int32_t* row = st + kSlotRows + 1 + r * kRowSlots; // sub_11027
    row[1] = item[0];
    std::memcpy(row + 2, item + 1, 32);
    row[10] = item[9], row[11] = item[10], row[12] = item[11];
    std::memcpy(row + 13, item + 12, 16);
    row[17] = item[16], row[18] = item[17];
    row[0] = 1;
  }
  st[kSlotSelected] = st[kSlotRows + 1 + st[kSlotCursor] * kRowSlots + 1];
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
    rebuild_display(st);
    return;
  }
  if (count < cap && !has_item(st, kIdMultiplayer)) {
    write_item(st, count, kIdMultiplayer, "CP_OPT_NETWORK");
    st[kSlotCount] = count + 1;
    rebuild_display(st);
  }
}

// Each message gets its own key and labels are never changed or freed afterwards: the
// help box (drawn on the render thread) may still hold a previous message's string.
void show_help(const std::string& text, bool forever) {
  static int serial = 0;
  const std::string key = std::format("IVC_M{}", ++serial);
  set_label(key, text);
  rehash_labels(); // shown right now, so it must resolve right now
  n::ClearHelp();
  if (forever)
    n::PrintHelpForever(key.c_str());
  else
    n::PrintHelp(key.c_str());
}

} // namespace

bool install() {
  set_label("CP_OPT_NETWORK", "Multiplayer");
  set_label("IVC_HOST", "Host Co-op");
  set_label("IVC_JOIN", "Join Co-op");
  set_label("IVC_LEAVE", "Leave Co-op");
  if (!hook(kTextGet, reinterpret_cast<void*>(&text_get_detour), reinterpret_cast<void**>(&g_text_get_orig),
            "CText::Get") ||
      !hook(kTextGetByHash, reinterpret_cast<void*>(&get_by_hash_detour),
            reinterpret_cast<void**>(&g_get_by_hash_orig), "CText::GetByHash") ||
      !hook(kTextExists, reinterpret_cast<void*>(&exists_detour), reinterpret_cast<void**>(&g_exists_orig),
            "CText::Exists"))
    return false;
  log::info("Phone: text lookups hooked");
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
  if (g_hash_dirty) rehash_labels();
  const std::int32_t s = screen();
  static std::int32_t last = -1;
  if (s != last) {
    log::debug("Phone: screen {} -> {}", last, s);
    last = s;
  }
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
      switch_screen(kScreenKeypad);
      show_help(std::format("Type a {}-digit co-op password, then press Call.", kPasswordDigits), true);
      log::info("Phone: {} chosen; waiting for the password", s == kIdHost ? "Host" : "Join");
      break;
    case kIdLeave:
      g_coop_menu = false;
      g_request = Request{Action::Leave, {}};
      switch_screen(kScreenClose);
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
  g_hash_dirty = true;
}

} // namespace ivc::phone
