#include "render/overlay.h"

#include <d3d9.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include "core/log.h"
#include "game/script.h"
#include "sync/ghost.h"

namespace ivc::overlay {
namespace {
bool g_visible = true;
int g_toggle_key = VK_F8;
// Starts "down" so a stale key state on the first poll can't toggle the overlay; the key must
// be seen released once before it does anything.
bool g_key_was_down = true;

HWND g_window = nullptr;
bool g_ready = false;
unsigned long long g_frames = 0;

void init_imgui(IDirect3DDevice9* dev) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr; // don't drop imgui.ini into the game folder
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  ImGui::StyleColorsDark();
  ImGui_ImplWin32_Init(g_window);
  ImGui_ImplDX9_Init(dev);
  g_ready = true;
  log::info("Overlay initialised (ImGui {})", IMGUI_VERSION);
}

int g_scripts_key = VK_F7;
bool g_scripts_was_down = true;
bool g_show_scripts = false;

// Rising-edge detector for a hotkey, only while the game window is focused.
bool pressed(int vk, bool& was_down) {
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool edge = down && !was_down && GetForegroundWindow() == g_window;
  was_down = down;
  return edge;
}

void poll_toggle() {
  if (pressed(g_toggle_key, g_key_was_down)) {
    g_visible = !g_visible;
    log::info("Overlay {}", g_visible ? "shown" : "hidden");
  }
  if (pressed(g_scripts_key, g_scripts_was_down)) g_show_scripts = !g_show_scripts;
}

constexpr ImGuiWindowFlags kHudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav;

void draw_status() {
  ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.55f);
  if (ImGui::Begin("##ivcoop", nullptr, kHudFlags)) {
    ImGui::TextUnformatted("iv-coop  M1");
    ImGui::Text("frame %llu  |  %.0f fps", g_frames, ImGui::GetIO().Framerate);

    const std::string host = script::host_script();
    if (host.empty())
      ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "script tick: waiting for a script to run");
    else
      ImGui::Text("script tick: host '%s'  (%zu scripts)", host.c_str(), script::threads().size());

    const ghost::Status g = ghost::status();
    ImGui::Text("player  %.1f %.1f %.1f  hdg %.0f", g.player_x, g.player_y, g.player_z, g.player_heading);
    ImGui::Separator();
    ImGui::Text("ghost [F9]: %s", g.enabled ? g.phase.c_str() : "off");
    if (g.enabled) {
      ImGui::Text("sent %u  recv %u  buffered %u  stale %u", g.sent, g.received, g.buffered, g.stale);
      const unsigned total = g.interpolated + g.extrapolated;
      const float extra_pct = total ? 100.0f * static_cast<float>(g.extrapolated) / static_cast<float>(total) : 0.0f;
      const ImVec4 col = extra_pct < 5.0f ? ImVec4(0.5f, 1, 0.5f, 1) : ImVec4(1, 0.6f, 0.3f, 1);
      ImGui::TextColored(col, "offset %d ms  |  extrapolated %.1f%% of frames", g.offset_ms, extra_pct);
    }
    ImGui::TextDisabled("F7 scripts  |  F8 overlay  |  built " __DATE__ " " __TIME__);
  }
  ImGui::End();
}

void draw_scripts() {
  ImGui::SetNextWindowPos(ImVec2(12, 220), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.7f);
  if (ImGui::Begin("##scripts", nullptr, kHudFlags)) {
    ImGui::TextUnformatted("Running scripts");
    ImGui::Separator();
    static const char* kStates[] = {"run", "blocked", "aborted", "halted"};
    for (const auto& t : script::threads()) {
      const char* st = (t.state >= 0 && t.state < 4) ? kStates[t.state] : "?";
      if (t.suppressed)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%5d  %-24s %s  SUPPRESSED", t.id, t.name.c_str(), st);
      else
        ImGui::Text("%5d  %-24s %s", t.id, t.name.c_str(), st);
    }
  }
  ImGui::End();
}

void draw() {
  draw_status();
  if (g_show_scripts) draw_scripts();
}
} // namespace

void configure(bool enabled, int toggle_key, int scripts_key) {
  g_visible = enabled;
  g_toggle_key = toggle_key;
  g_scripts_key = scripts_key;
}

void set_window(HWND window) {
  if (!g_ready) g_window = window;
}

void on_present(IDirect3DDevice9* dev) {
  ++g_frames;
  if (!g_ready) {
    if (!g_window) return;
    init_imgui(dev);
  }
  if (g_frames == 1 || g_frames % 3600 == 0) log::debug("Present frame {}", g_frames);

  poll_toggle();
  if (!g_visible) return;

  ImGui_ImplDX9_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  draw();
  ImGui::EndFrame();

  if (SUCCEEDED(dev->BeginScene())) {
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    dev->EndScene();
  }
}

void on_before_reset() {
  if (g_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
}

void on_after_reset() {
  if (g_ready) ImGui_ImplDX9_CreateDeviceObjects();
}

} // namespace ivc::overlay
