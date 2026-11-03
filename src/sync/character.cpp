#include "sync/character.h"

#include <windows.h>

#include <algorithm>
#include <format>
#include <map>

#include "core/log.h"
#include "game/model_swap.h"
#include "net/bytes.h"

namespace ivc::character {
namespace {
namespace n = natives;

constexpr DWORD kResendMs = 5000;
// Prop variation counts have no native; an index the engine refuses is detected by reading it back, and these
// stop the cycling short of anything absurd. Textures can't be read back, so they stay small.
constexpr int kMaxPropIndex = 24, kMaxPropTex = 4;

const char* const kCompNames[kComponents] = {"Head", "Torso", "Legs", "Special", "Hands", "Feet", "Jacket", "Hair", "Extra"};
const char* const kPropNames[kProps] = {"Hat", "Glasses"};

std::wstring g_ini;
Character g_saved;
bool g_configured = false;
std::uint32_t g_version = 0; // bumped on every save (the sender resends)

std::uint32_t male_hash() {
  static const std::uint32_t h = n::GetHashKey("M_Y_MULTIPLAYER");
  return h;
}
std::uint32_t female_hash() {
  static const std::uint32_t h = n::GetHashKey("F_Y_MULTIPLAYER");
  return h;
}

n::Ped player_ped() {
  const n::Ped p = n::GetPlayerChar(n::GetPlayerId());
  return p && n::DoesCharExist(p) ? p : 0;
}

// Wears `c` on `ped` (which already has c's model). Every value is checked against what the model has, since
// a character can come from a peer's file or packet.
void apply(n::Ped ped, const Character& c) {
  for (int comp = 0; comp < kComponents; ++comp) {
    const std::uint32_t drawables = n::GetNumberOfCharDrawableVariations(ped, comp);
    if (drawables == 0) continue;
    const std::uint32_t d = c.drawable[comp] < drawables ? c.drawable[comp] : 0;
    const std::uint32_t textures = n::GetNumberOfCharTextureVariations(ped, comp, d);
    const std::uint32_t t = c.texture[comp] < textures ? c.texture[comp] : 0;
    n::SetCharComponentVariation(ped, comp, d, t);
  }
  for (int p = 0; p < kProps; ++p) {
    if (c.prop[p] < 0 || c.prop[p] > kMaxPropIndex) {
      n::ClearCharProp(ped, p);
      continue;
    }
    n::SetCharPropIndex(ped, p, c.prop[p]);
    if (n::GetCharPropIndex(ped, p) != c.prop[p]) {
      n::ClearCharProp(ped, p); // the model doesn't have that prop
      continue;
    }
    if (c.prop_tex[p] > 0 && c.prop_tex[p] < kMaxPropTex) n::SetCharPropIndexTexture(ped, p, c.prop[p], c.prop_tex[p]);
  }
}

Character from_ped(n::Ped ped) {
  Character c;
  c.female = n::GetCharModel(ped) == female_hash();
  for (int comp = 0; comp < kComponents; ++comp) {
    c.drawable[comp] = static_cast<std::uint8_t>(n::GetCharDrawableVariation(ped, comp));
    c.texture[comp] = static_cast<std::uint8_t>(n::GetCharTextureVariation(ped, comp));
  }
  return c;
}

void save_file() {
  if (g_ini.empty()) return;
  auto put = [](const std::wstring& key, int v) {
    WritePrivateProfileStringW(L"Character", key.c_str(), std::to_wstring(v).c_str(), g_ini.c_str());
  };
  put(L"Configured", 1);
  put(L"Female", g_saved.female);
  for (int i = 0; i < kComponents; ++i) {
    put(L"Drawable" + std::to_wstring(i), g_saved.drawable[i]);
    put(L"Texture" + std::to_wstring(i), g_saved.texture[i]);
  }
  put(L"Hat", g_saved.prop[0] + 1); // 0: none
  put(L"HatTexture", g_saved.prop_tex[0]);
  put(L"Glasses", g_saved.prop[1] + 1);
  put(L"GlassesTexture", g_saved.prop_tex[1]);
}

// ---- editor

enum class Want { None, Draft, Saved, Original };
bool g_editing = false;
Character g_draft;
Want g_want = Want::None;
bool g_seed = false;       // after the next model swap, take the model's default look as the draft
bool g_dirty = false;      // rows changed by themselves
bool g_joined_ped = false; // set by tick: our ped is the joiner's (a session is live)
// What the player wore when the editor opened (restored by Back / after Save outside a session).
std::uint32_t g_open_model = 0;
std::uint8_t g_open_drawable[n::kPedComponents] = {}, g_open_texture[n::kPedComponents] = {};

// Puts the model for g_want on the player and dresses it; false while the model streams in.
bool settle() {
  n::Ped ped = player_ped();
  if (!ped) return false;
  const std::uint32_t target = g_want == Want::Draft   ? model_hash(g_draft)
                               : g_want == Want::Saved ? model_hash(g_saved)
                                                       : g_open_model;
  if (n::GetCharModel(ped) != target) {
    n::RequestModel(target);
    if (!n::HasModelLoaded(target)) return false;
    const n::Player player = n::GetPlayerId();
    if (!model_swap::change_player_model(player, target)) return false; // hidden for a few frames first
    ped = n::GetPlayerChar(player);
    if (!ped) return false;
    n::SetCharDefaultComponentVariation(ped);
    n::MarkModelAsNoLongerNeeded(target);
    log::info("Character: preview model 0x{:08X}", target);
  }
  switch (g_want) {
    case Want::Draft:
      if (g_seed) {
        const bool female = g_draft.female;
        g_draft = from_ped(ped);
        g_draft.female = female;
        g_seed = false;
      } else {
        apply(ped, g_draft);
      }
      break;
    case Want::Saved: apply(ped, g_saved); break;
    default:
      for (std::uint32_t c = 0; c < n::kPedComponents; ++c)
        n::SetCharComponentVariation(ped, c, g_open_drawable[c], g_open_texture[c]);
      break;
  }
  g_want = Want::None;
  g_dirty = true;
  return true;
}

enum class Kind { Loading, Model, Drawable, Texture, Prop, PropTex };
struct RowDef {
  Kind kind;
  int index;
  std::string text;
};

std::vector<RowDef> build_rows() {
  std::vector<RowDef> rows;
  const n::Ped ped = player_ped();
  if (g_want != Want::None || !ped || n::GetCharModel(ped) != model_hash(g_draft)) {
    rows.push_back({Kind::Loading, 0, "Loading..."});
    return rows;
  }
  rows.push_back({Kind::Model, 0, std::format("Model: {}", g_draft.female ? "Female" : "Male")});
  for (int c = 0; c < kComponents; ++c) {
    const std::uint32_t drawables = n::GetNumberOfCharDrawableVariations(ped, c);
    if (drawables > 1)
      rows.push_back({Kind::Drawable, c, std::format("{}: {}/{}", kCompNames[c], g_draft.drawable[c] + 1, drawables)});
    const std::uint32_t textures = drawables ? n::GetNumberOfCharTextureVariations(ped, c, g_draft.drawable[c]) : 0;
    if (textures > 1)
      rows.push_back({Kind::Texture, c, std::format("{} tex: {}/{}", kCompNames[c], g_draft.texture[c] + 1, textures)});
  }
  for (int p = 0; p < kProps; ++p) {
    rows.push_back({Kind::Prop, p,
                    g_draft.prop[p] < 0 ? std::format("{}: None", kPropNames[p])
                                        : std::format("{}: {}", kPropNames[p], g_draft.prop[p] + 1)});
    if (g_draft.prop[p] >= 0)
      rows.push_back({Kind::PropTex, p, std::format("{} tex: {}", kPropNames[p], g_draft.prop_tex[p] + 1)});
  }
  return rows;
}

// ---- sync

struct RemoteChar {
  Character ch;
  n::Ped applied = 0;
  bool dirty = true;
};
std::map<std::uint8_t, RemoteChar> g_remote;

bool g_was_active = false;
std::uint32_t g_sent_version = 0;
std::vector<std::uint8_t> g_sent_players;
DWORD g_last_send = 0;

void send_own(net::Session& net) {
  net::Writer w;
  w.put(kMsg).put(std::uint8_t{0}).put(model_hash(g_saved));
  for (int i = 0; i < kComponents; ++i) w.put(g_saved.drawable[i]);
  for (int i = 0; i < kComponents; ++i) w.put(g_saved.texture[i]);
  for (int i = 0; i < kProps; ++i) w.put(g_saved.prop[i]);
  for (int i = 0; i < kProps; ++i) w.put(g_saved.prop_tex[i]);
  net.send_game(std::move(w.data()), true);
}

} // namespace

void configure(const std::filesystem::path& plugin_dir) {
  std::error_code ec;
  std::filesystem::create_directories(plugin_dir / "iv-coop", ec);
  g_ini = (plugin_dir / "iv-coop" / "character.ini").wstring();
  auto get = [](const std::wstring& key, int def) {
    return static_cast<int>(GetPrivateProfileIntW(L"Character", key.c_str(), def, g_ini.c_str()));
  };
  if (!get(L"Configured", 0)) return;
  g_saved.female = get(L"Female", 0) != 0;
  for (int i = 0; i < kComponents; ++i) {
    g_saved.drawable[i] = static_cast<std::uint8_t>(std::clamp(get(L"Drawable" + std::to_wstring(i), 0), 0, 255));
    g_saved.texture[i] = static_cast<std::uint8_t>(std::clamp(get(L"Texture" + std::to_wstring(i), 0), 0, 255));
  }
  g_saved.prop[0] = static_cast<std::int8_t>(std::clamp(get(L"Hat", 0), 0, kMaxPropIndex + 1) - 1);
  g_saved.prop_tex[0] = static_cast<std::uint8_t>(std::clamp(get(L"HatTexture", 0), 0, kMaxPropTex - 1));
  g_saved.prop[1] = static_cast<std::int8_t>(std::clamp(get(L"Glasses", 0), 0, kMaxPropIndex + 1) - 1);
  g_saved.prop_tex[1] = static_cast<std::uint8_t>(std::clamp(get(L"GlassesTexture", 0), 0, kMaxPropTex - 1));
  g_configured = true;
}

bool configured() { return g_configured; }

std::uint32_t model_hash(const Character& c) { return c.female ? female_hash() : male_hash(); }

std::uint32_t joiner_model(std::uint32_t fallback) { return g_configured ? model_hash(g_saved) : fallback; }

bool is_own_model(std::uint32_t model) { return g_configured && model == model_hash(g_saved); }

void apply_own(n::Ped ped) {
  if (ped && g_configured) apply(ped, g_saved);
}

bool editor_active() { return g_editing; }

bool editor_open() {
  const n::Ped ped = player_ped();
  if (!ped) return false;
  g_open_model = n::GetCharModel(ped);
  for (std::uint32_t c = 0; c < n::kPedComponents; ++c) {
    g_open_drawable[c] = static_cast<std::uint8_t>(n::GetCharDrawableVariation(ped, c));
    g_open_texture[c] = static_cast<std::uint8_t>(n::GetCharTextureVariation(ped, c));
  }
  g_draft = g_configured ? g_saved : Character{};
  g_seed = !g_configured;
  if (!g_configured) g_draft.female = g_open_model == female_hash();
  g_editing = true;
  g_want = Want::Draft;
  settle();
  log::info("Character: editor opened ({}), wearing 0x{:08X}", g_configured ? "saved character" : "no character yet",
            g_open_model);
  return true;
}

void editor_close(bool save) {
  if (!g_editing) return;
  g_editing = false;
  g_seed = false;
  if (save) {
    g_saved = g_draft;
    g_configured = true;
    ++g_version;
    save_file();
    log::info("Character: saved ({}, hat {}, glasses {})", g_saved.female ? "female" : "male", g_saved.prop[0],
              g_saved.prop[1]);
    // In a session the ped keeps the new look; otherwise the player goes back to who they were.
    g_want = g_joined_ped ? Want::Draft : Want::Original;
  } else {
    g_want = g_joined_ped && g_configured ? Want::Saved : Want::Original;
  }
  settle();
}

std::vector<std::string> editor_rows() {
  std::vector<std::string> out;
  for (const RowDef& r : build_rows()) out.push_back(r.text);
  return out;
}

void editor_activate(int row) {
  const n::Ped ped = player_ped();
  const std::vector<RowDef> rows = build_rows();
  if (!g_editing || !ped || row < 0 || row >= static_cast<int>(rows.size())) return;
  const RowDef& r = rows[row];
  const int i = r.index;
  switch (r.kind) {
    case Kind::Model: {
      const bool female = !g_draft.female;
      g_draft = Character{};
      g_draft.female = female;
      g_seed = true; // the other model's default look becomes the draft
      g_want = Want::Draft;
      settle();
      break;
    }
    case Kind::Drawable: {
      const std::uint32_t count = std::max<std::uint32_t>(1, n::GetNumberOfCharDrawableVariations(ped, i));
      g_draft.drawable[i] = static_cast<std::uint8_t>((g_draft.drawable[i] + 1) % count);
      g_draft.texture[i] = 0;
      n::SetCharComponentVariation(ped, i, g_draft.drawable[i], 0);
      break;
    }
    case Kind::Texture: {
      const std::uint32_t count =
          std::max<std::uint32_t>(1, n::GetNumberOfCharTextureVariations(ped, i, g_draft.drawable[i]));
      g_draft.texture[i] = static_cast<std::uint8_t>((g_draft.texture[i] + 1) % count);
      n::SetCharComponentVariation(ped, i, g_draft.drawable[i], g_draft.texture[i]);
      break;
    }
    case Kind::Prop: {
      int next = g_draft.prop[i] + 1;
      if (next > kMaxPropIndex) next = -1;
      if (next >= 0) {
        n::SetCharPropIndex(ped, i, next);
        if (n::GetCharPropIndex(ped, i) != next) next = -1; // past the model's last prop: back to none
      }
      if (next < 0) n::ClearCharProp(ped, i);
      g_draft.prop[i] = static_cast<std::int8_t>(next);
      g_draft.prop_tex[i] = 0;
      break;
    }
    case Kind::PropTex:
      g_draft.prop_tex[i] = static_cast<std::uint8_t>((g_draft.prop_tex[i] + 1) % kMaxPropTex);
      n::SetCharPropIndexTexture(ped, i, g_draft.prop[i], g_draft.prop_tex[i]);
      break;
    default: break;
  }
}

bool editor_take_dirty() { return std::exchange(g_dirty, false); }

void tick(const Frame& f, net::Session& net) {
  g_joined_ped = f.joiner_active;
  if (g_want != Want::None) settle();

  const bool active = f.in_session && f.joiner_active && g_configured && !g_editing;
  if (!active) {
    g_was_active = false;
    return;
  }
  const DWORD now = GetTickCount();
  if (g_was_active && g_sent_version == g_version && g_sent_players == f.players && now - g_last_send < kResendMs) return;
  g_was_active = true, g_sent_version = g_version, g_sent_players = f.players, g_last_send = now;
  send_own(net);
}

void on_message(const net::GameMessage& m) {
  net::Reader r(m.data);
  std::uint8_t type, sender;
  std::uint32_t model;
  Character c;
  if (!r.get(type) || !r.get(sender) || !r.get(model)) return;
  if (model != male_hash() && model != female_hash()) return;
  c.female = model == female_hash();
  for (int i = 0; i < kComponents; ++i)
    if (!r.get(c.drawable[i])) return;
  for (int i = 0; i < kComponents; ++i)
    if (!r.get(c.texture[i])) return;
  for (int i = 0; i < kProps; ++i)
    if (!r.get(c.prop[i])) return;
  for (int i = 0; i < kProps; ++i)
    if (!r.get(c.prop_tex[i])) return;
  RemoteChar& rc = g_remote[m.from];
  if (rc.ch == c) return;
  rc.ch = c, rc.dirty = true;
  log::info("Character: player #{} is {}", m.from, c.female ? "female" : "male");
}

void on_puppet(std::uint8_t id, n::Ped ped) {
  const auto it = g_remote.find(id);
  if (it == g_remote.end() || !ped || !n::DoesCharExist(ped)) return;
  RemoteChar& rc = it->second;
  if (rc.applied == ped && !rc.dirty) return;
  if (n::GetCharModel(ped) != model_hash(rc.ch)) return; // the copy is still the other model; wait for it
  apply(ped, rc.ch);
  rc.applied = ped, rc.dirty = false;
}

void forget(std::uint8_t id) { g_remote.erase(id); }

} // namespace ivc::character
