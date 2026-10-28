#include "story/hud.h"

#include <windows.h>

#include <array>
#include <map>
#include <string>
#include <vector>

#include "game/native_hooks.h"
#include "game/natives.h"
#include "net/bytes.h"
#include "story/story_protocol.h"
#include "story/story_sync.h"

namespace ivc::story::hud {
namespace {
using native_hooks::Call;

constexpr int kMaxArgs = 10, kMaxStrings = 3;
constexpr DWORD kSendMs = 100, kStaleMs = 600;
constexpr std::uint32_t kSetTextRenderId = 0x2B1B0290;

// What we capture: the hash, its argument count and which arguments are strings (GXT keys
// or literal strings; everything else is a 32-bit int/float slot).
struct Kind {
  std::uint32_t hash;
  const char* name;
  int args;
  std::array<int, kMaxStrings> strings; // arg indices, -1 = none
};
constexpr Kind kKinds[] = {
    {0x19C967B5, "SET_TEXT_COLOUR", 4, {-1, -1, -1}},
    {0x02C069E5, "SET_TEXT_SCALE", 2, {-1, -1, -1}},
    {0x19D006EB, "SET_TEXT_WRAP", 2, {-1, -1, -1}},
    {0x75363BB5, "SET_TEXT_FONT", 1, {-1, -1, -1}},
    {0x58F5023F, "SET_TEXT_DROPSHADOW", 5, {-1, -1, -1}},
    {0x748B78B6, "SET_TEXT_RIGHT_JUSTIFY", 1, {-1, -1, -1}},
    {0x204A6AA4, "SET_TEXT_CENTRE", 1, {-1, -1, -1}},
    {0x768F5140, "SET_TEXT_BACKGROUND", 1, {-1, -1, -1}},
    {0x2D7A725D, "SET_TEXT_EDGE", 5, {-1, -1, -1}},
    {0x6CFD0610, "SET_TEXT_DRAW_BEFORE_FADE", 1, {-1, -1, -1}},
    {0x15585A65, "SET_TEXT_PROPORTIONAL", 1, {-1, -1, -1}},
    {0x049D23F9, "SET_TEXT_JUSTIFY", 1, {-1, -1, -1}},
    {0x1F6A54B6, "SET_TEXT_LINE_DISPLAY", 2, {-1, -1, -1}},
    {0x0AD54D75, "SET_TEXT_USE_UNDERSCORE", 1, {-1, -1, -1}},
    {0x2F9E362B, "SET_TEXT_CENTRE_WRAPX", 1, {-1, -1, -1}},
    {0x0F002557, "DISPLAY_TEXT", 3, {2, -1, -1}},
    {0x5A495ABE, "DISPLAY_TEXT_WITH_NUMBER", 4, {2, -1, -1}},
    {0x311F4FE9, "DISPLAY_TEXT_WITH_FLOAT", 5, {2, -1, -1}},
    {0x661B239A, "DISPLAY_TEXT_WITH_LITERAL_STRING", 4, {2, 3, -1}},
    {0x10A75905, "DISPLAY_TEXT_WITH_STRING", 4, {2, 3, -1}},
    {0x369A4540, "DISPLAY_TEXT_WITH_STRING_AND_INT", 5, {2, 3, -1}},
    {0x66842574, "DISPLAY_TEXT_WITH_TWO_STRINGS", 5, {2, 3, 4}},
    {0x4B7C3AEC, "DISPLAY_TEXT_WITH_TWO_LITERAL_STRINGS", 5, {2, 3, 4}},
    {0x7EF6599D, "DISPLAY_TEXT_WITH_SUBSTRING_GIVEN_HASH_KEY", 4, {2, -1, -1}},
    {0x39E77F70, "DISPLAY_TEXT_WITH_TWO_SUBSTRINGS_GIVEN_HASH_KEYS", 5, {2, -1, -1}},
    {0x3B2526E3, "DRAW_RECT", 8, {-1, -1, -1}},
    // The episodes' activity screens (base jumps, races, drug wars, fight club) and race markers drawn each frame.
    {0x337957AF, "DISPLAY_TEXT_WITH_2_NUMBERS", 5, {2, -1, -1}},
    {0x746C06E8, "DISPLAY_TEXT_WITH_3_NUMBERS", 6, {2, -1, -1}},
    {0x29FC3E19, "DRAW_CHECKPOINT", 7, {-1, -1, -1}},
    {0x26810BE3, "DRAW_CHECKPOINT_WITH_ALPHA", 8, {-1, -1, -1}},
};

struct Command {
  std::uint32_t hash = 0;
  std::uint8_t argc = 0;
  std::array<std::uint32_t, kMaxArgs> args{};
  std::array<std::int8_t, kMaxStrings> string_arg{-1, -1, -1};
  std::array<Key, kMaxStrings> strings{};
};

// Host.
bool g_hosting = false;
std::vector<Command> g_frame, g_latest;
std::vector<std::string> g_render_target_scripts; // scripts drawing to an in-world screen this frame
DWORD g_last_send = 0;
bool g_sent_empty = true;

// Joiner.
std::vector<Command> g_draw;
DWORD g_draw_at = 0;

// Sprites (minigame screens: darts, pool, air hockey, golf, cards, the episodes' activity screens). A texture handle
// means nothing on another machine, so the host remembers which dictionary and name each came from and sends those
// (as the command's three strings: dictionary, texture, "s" streamed / "t" loaded); partners load the same texture
// and draw it with their own handle.
constexpr std::uint32_t kLoadTxd = 0x52FC763A, kGetTxd = 0x15D668D0, kGetTexture = 0x0F5D1937,
                        kGetStreamedTexture = 0x32C24491, kRequestStreamedTxd = 0x7C7B1237,
                        kHasStreamedTxdLoaded = 0x5F9C43D4;
constexpr std::uint32_t kSpriteKinds[] = {0x6ADD40EC /* DRAW_SPRITE */, 0x1849408D /* DRAW_TOPLEVEL_SPRITE */};
struct TextureName {
  std::string txd, name;
  bool streamed;
};
std::map<std::uint32_t, std::string> g_txd_names;  // host: dictionary handle -> name
std::map<std::uint32_t, TextureName> g_textures;   // host: texture handle -> where it came from
std::map<std::string, std::uint32_t> g_resolved;   // joiner: "s|txd|name" -> our texture handle (0: loading)

bool is_sprite(std::uint32_t hash) {
  for (std::uint32_t h : kSpriteKinds)
    if (h == hash) return true;
  return false;
}

void capture_sprite(const Call& c, std::uint32_t hash) {
  if (!g_hosting || !is_story_script(c.script)) return;
  for (const auto& s : g_render_target_scripts)
    if (s == c.script) return;
  const auto it = g_textures.find(c.arg<std::uint32_t>(0));
  if (it == g_textures.end()) return;
  Command cmd;
  cmd.hash = hash;
  cmd.argc = 10;
  for (int i = 0; i < 10; ++i) cmd.args[i] = c.arg<std::uint32_t>(i);
  cmd.string_arg = {0, 0, 0};
  cmd.strings = {make_key(it->second.txd), make_key(it->second.name), make_key(it->second.streamed ? "s" : "t")};
  if (g_frame.size() < 256) g_frame.push_back(cmd);
}

std::uint32_t resolve_texture(const std::string& txd, const std::string& name, bool streamed) {
  const std::string key = (streamed ? "s|" : "t|") + txd + "|" + name;
  if (const auto it = g_resolved.find(key); it != g_resolved.end() && it->second) return it->second;
  std::uint32_t tex = 0;
  if (streamed) {
    natives::invoke(kRequestStreamedTxd, txd.c_str(), true);
    if (natives::invoke<bool>(kHasStreamedTxdLoaded, txd.c_str()))
      tex = natives::invoke<std::uint32_t>(kGetStreamedTexture, txd.c_str(), name.c_str());
  } else if (const auto dict = natives::invoke<std::uint32_t>(kLoadTxd, txd.c_str())) {
    tex = natives::invoke<std::uint32_t>(kGetTexture, dict, name.c_str());
  }
  g_resolved[key] = tex;
  return tex;
}

void capture(const Call& c, const Kind& k) {
  if (!g_hosting || !is_story_script(c.script)) return;
  for (const auto& s : g_render_target_scripts)
    if (s == c.script) return;
  Command cmd;
  cmd.hash = k.hash;
  cmd.argc = static_cast<std::uint8_t>(k.args);
  for (int i = 0; i < k.args; ++i) cmd.args[i] = c.arg<std::uint32_t>(i);
  for (int s = 0; s < kMaxStrings; ++s)
    if (k.strings[s] >= 0) {
      cmd.string_arg[s] = static_cast<std::int8_t>(k.strings[s]);
      cmd.strings[s] = make_key(c.str(k.strings[s]));
    }
  if (g_frame.size() < 256) g_frame.push_back(cmd);
}

std::vector<std::uint8_t> encode(const std::vector<Command>& list) {
  net::Writer w;
  w.put(StoryMsg::Hud).put(std::uint8_t{0}).put(static_cast<std::uint16_t>(list.size()));
  for (const Command& c : list) {
    w.put(c.hash).put(c.argc);
    for (int i = 0; i < c.argc; ++i) w.put(c.args[i]);
    for (int s = 0; s < kMaxStrings; ++s) {
      w.put(c.string_arg[s]);
      if (c.string_arg[s] >= 0) w.put(c.strings[s]);
    }
  }
  return std::move(w.data());
}

} // namespace

void register_hooks() {
  for (const Kind& k : kKinds) native_hooks::on(k.hash, k.name, [&k](const Call& c) { capture(c, k); });
  auto txd = [](const Call& c) {
    if (g_hosting) g_txd_names[c.result<std::uint32_t>()] = c.str(0);
  };
  native_hooks::on(kLoadTxd, "LOAD_TXD", txd);
  native_hooks::on(kGetTxd, "GET_TXD", txd);
  native_hooks::on(kGetTexture, "GET_TEXTURE", [](const Call& c) {
    if (!g_hosting) return;
    const auto it = g_txd_names.find(c.arg<std::uint32_t>(0));
    if (it != g_txd_names.end()) g_textures[c.result<std::uint32_t>()] = {it->second, c.str(1), false};
  });
  native_hooks::on(kGetStreamedTexture, "GET_TEXTURE_FROM_STREAMED_TXD", [](const Call& c) {
    if (g_hosting) g_textures[c.result<std::uint32_t>()] = {c.str(0), c.str(1), true};
  });
  native_hooks::on(kSpriteKinds[0], "DRAW_SPRITE", [](const Call& c) { capture_sprite(c, kSpriteKinds[0]); });
  native_hooks::on(kSpriteKinds[1], "DRAW_TOPLEVEL_SPRITE", [](const Call& c) { capture_sprite(c, kSpriteKinds[1]); });
  native_hooks::on(kSetTextRenderId, "SET_TEXT_RENDER_ID", [](const Call& c) {
    if (g_hosting && is_story_script(c.script)) g_render_target_scripts.push_back(c.script);
  });
}

void host_tick(bool hosting, net::Session& net) {
  g_hosting = hosting;
  g_latest = std::move(g_frame); // everything the scripts drew this frame
  g_frame.clear();
  g_render_target_scripts.clear();
  if (!hosting) return;
  const DWORD now = GetTickCount();
  if (now - g_last_send < kSendMs) return;
  if (g_latest.empty() && g_sent_empty) return;
  g_last_send = now;
  g_sent_empty = g_latest.empty();
  net.send_game(encode(g_latest), false);
}

void client_apply(std::span<const std::uint8_t> msg) {
  net::Reader r(msg);
  StoryMsg type;
  std::uint8_t sender;
  std::uint16_t count = 0;
  if (!r.get(type) || !r.get(sender) || !r.get(count)) return;
  std::vector<Command> list;
  for (int i = 0; i < count && r.ok(); ++i) {
    Command c;
    r.get(c.hash), r.get(c.argc);
    if (c.argc > kMaxArgs) return;
    for (int a = 0; a < c.argc; ++a) r.get(c.args[a]);
    for (int s = 0; s < kMaxStrings; ++s) {
      r.get(c.string_arg[s]);
      if (c.string_arg[s] >= 0) r.get(c.strings[s]);
    }
    list.push_back(c);
  }
  if (!r.ok()) return;
  g_draw = std::move(list);
  g_draw_at = GetTickCount();
}

void client_tick() {
  if (g_draw.empty()) return;
  if (GetTickCount() - g_draw_at > kStaleMs) {
    g_draw.clear();
    return;
  }
  for (const Command& c : g_draw) {
    std::array<std::uint32_t, kMaxArgs> args = c.args;
    if (is_sprite(c.hash)) {
      args[0] = resolve_texture(to_string(c.strings[0]), to_string(c.strings[1]), to_string(c.strings[2]) == "s");
      if (args[0]) natives::invoke_raw(c.hash, args.data(), c.argc);
      continue;
    }
    std::array<std::string, kMaxStrings> text;
    for (int s = 0; s < kMaxStrings; ++s)
      if (c.string_arg[s] >= 0 && c.string_arg[s] < c.argc) {
        text[s] = to_string(c.strings[s]);
        args[c.string_arg[s]] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(text[s].c_str()));
      }
    natives::invoke_raw(c.hash, args.data(), c.argc);
  }
}

void clear() {
  g_resolved.clear(); // handles from an older world (or session) may be stale
  g_frame.clear();
  g_latest.clear();
  g_draw.clear();
  g_sent_empty = true;
}

} // namespace ivc::story::hud
