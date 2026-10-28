#include "render/banner.h"

#include <mmsystem.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <string>

#include "core/log.h"
#include "game/natives.h"
#include "render/banner_res.h"

namespace ivc::banner {
namespace {
namespace n = natives;

constexpr float kFadeIn = 0.3f, kHold = 3.0f, kFadeOut = 1.0f;
constexpr float kFontPx = 112.0f;
constexpr std::uint32_t kPlayFrontend = 0x4DAF2C87, kEvent = 0x486F3D93, kMissionComplete = 0x4BAF0213;

HMODULE g_self = nullptr;
std::filesystem::path g_ui_dir;

struct Shown {
  bool on = false;
  Kind kind = Kind::Victory;
  std::string text;
  ULONGLONG start_ms = 0;
};
std::mutex g_lock;
Shown g_shown;
std::atomic<bool> g_active{false};

ImFont* g_font = nullptr;
bool g_font_tried = false;

const wchar_t* wav_name(Kind k) {
  return k == Kind::Invading ? L"banner_invading.wav" : k == Kind::Victory ? L"banner_victory.wav" : L"banner_defeat.wav";
}

// The user's WAV if they dropped one in, else a game stinger (frontend sounds, ids from the mission scripts).
void play_sound(Kind kind) {
  const std::filesystem::path wav = g_ui_dir / wav_name(kind);
  std::error_code ec;
  if (!g_ui_dir.empty() && std::filesystem::exists(wav, ec)) {
    if (PlaySoundW(wav.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) return;
  }
  switch (kind) {
    case Kind::Victory:
      n::invoke(kMissionComplete, 50); // the "mission passed" stinger
      break;
    case Kind::Defeat:
      n::invoke(kPlayFrontend, -1, "R4_BLEED_OUT_BODY_SPLASH"); // a heavy thud (mission-bank sound; may be silent)
      n::invoke(kPlayFrontend, -1, "POLICE_COMPUTER_SEARCH_FAIL");
      break;
    case Kind::Invading:
      n::invoke(kEvent, "FRONTEND_OTHER_RACE_321");
      break;
  }
}

float smooth(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

} // namespace

void configure(HMODULE self, const std::filesystem::path& plugin_dir) {
  g_self = self;
  g_ui_dir = plugin_dir / "iv-coop" / "ui";
}

void show(Kind kind, const char* text) {
  {
    std::lock_guard lk(g_lock);
    g_shown = {true, kind, text, GetTickCount64()};
    g_active = true;
  }
  log::info("Banner: {}", text);
  play_sound(kind);
}

bool active() { return g_active; }

void load_font() {
  if (g_font_tried || !ImGui::GetCurrentContext()) return;
  g_font_tried = true;
  HRSRC res = FindResourceW(g_self, MAKEINTRESOURCEW(IDR_BANNER_FONT), RT_RCDATA);
  HGLOBAL mem = res ? LoadResource(g_self, res) : nullptr;
  const void* data = mem ? LockResource(mem) : nullptr;
  if (!data) {
    log::warn("Banner: the embedded font is missing; using the default");
    return;
  }
  static const ImWchar kRanges[] = {0x20, 0x7E, 0};
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false; // the resource stays mapped for the life of the module
  cfg.OversampleH = 2, cfg.OversampleV = 1;
  g_font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<void*>(data), static_cast<int>(SizeofResource(g_self, res)),
                                                      kFontPx, &cfg, kRanges);
  if (!g_font) log::warn("Banner: could not load the embedded font");
}

void draw() {
  Shown s;
  {
    std::lock_guard lk(g_lock);
    s = g_shown;
  }
  if (!s.on) return;
  const float t = static_cast<float>(GetTickCount64() - s.start_ms) / 1000.0f;
  if (t >= kFadeIn + kHold + kFadeOut) {
    std::lock_guard lk(g_lock);
    g_shown.on = false;
    g_active = false;
    return;
  }
  const float a = t < kFadeIn ? smooth(t / kFadeIn) : t < kFadeIn + kHold ? 1.0f : 1.0f - smooth((t - kFadeIn - kHold) / kFadeOut);

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  if (screen.x < 16 || screen.y < 16) return;
  ImFont* font = g_font ? g_font : ImGui::GetFont();
  const float size = std::max(28.0f, screen.y * 0.085f);
  const float cy = screen.y * 0.5f, half = size * 1.15f, feather = size * 0.9f;

  // The band: solid dark middle, feathered top and bottom edges.
  const ImU32 dark = IM_COL32(0, 0, 0, static_cast<int>(165 * a)), clear = IM_COL32(0, 0, 0, 0);
  dl->AddRectFilledMultiColor({0, cy - half - feather}, {screen.x, cy - half}, clear, clear, dark, dark);
  dl->AddRectFilled({0, cy - half}, {screen.x, cy + half}, dark);
  dl->AddRectFilledMultiColor({0, cy + half}, {screen.x, cy + half + feather}, dark, dark, clear, clear);

  // The text, letter by letter so it can be spaced out; the spacing drifts wider while it is up.
  const float spacing = size * (0.20f + 0.03f * std::min(t / (kFadeIn + kHold + kFadeOut), 1.0f));
  float width = 0;
  for (std::size_t i = 0; i < s.text.size(); ++i)
    width += font->CalcTextSizeA(size, FLT_MAX, 0, &s.text[i], &s.text[i] + 1).x + (i + 1 < s.text.size() ? spacing : 0);
  float x = (screen.x - width) * 0.5f;
  const float y = cy - size * 0.5f;
  const bool good = s.kind != Kind::Defeat;
  const int r = good ? 222 : 190, g = good ? 182 : 28, b = good ? 92 : 24;
  const ImU32 col = IM_COL32(r, g, b, static_cast<int>(255 * a));
  const ImU32 glow = IM_COL32(r, g, b, static_cast<int>(48 * a));
  const ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(200 * a));
  for (std::size_t i = 0; i < s.text.size(); ++i) {
    const char* c = &s.text[i];
    const float adv = font->CalcTextSizeA(size, FLT_MAX, 0, c, c + 1).x;
    dl->AddText(font, size * 1.04f, {x - size * 0.02f, y - size * 0.02f}, glow, c, c + 1);
    dl->AddText(font, size, {x + size * 0.03f, y + size * 0.04f}, shadow, c, c + 1);
    dl->AddText(font, size, {x, y}, col, c, c + 1);
    x += adv + spacing;
  }
  // Thin rules above and below the lettering, like the game's.
  const float rule_w = std::min(screen.x * 0.5f, width * 1.15f);
  const ImU32 rule = IM_COL32(r, g, b, static_cast<int>(110 * a));
  dl->AddLine({(screen.x - rule_w) * 0.5f, cy - half * 0.9f}, {(screen.x + rule_w) * 0.5f, cy - half * 0.9f}, rule, 1.5f);
  dl->AddLine({(screen.x - rule_w) * 0.5f, cy + half * 0.9f}, {(screen.x + rule_w) * 0.5f, cy + half * 0.9f}, rule, 1.5f);
}

} // namespace ivc::banner
