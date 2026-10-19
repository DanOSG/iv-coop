#include "render/overlay.h"

#include <d3d9.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include "core/log.h"

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

void poll_toggle() {
  const bool down = (GetAsyncKeyState(g_toggle_key) & 0x8000) != 0;
  if (down && !g_key_was_down && GetForegroundWindow() == g_window) {
    g_visible = !g_visible;
    log::info("Overlay {}", g_visible ? "shown" : "hidden");
  }
  g_key_was_down = down;
}

void draw() {
  ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.55f);
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                                     ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##ivcoop", nullptr, flags)) {
    ImGui::TextUnformatted("iv-coop  M0");
    ImGui::Text("frame %llu  |  %.0f fps", g_frames, ImGui::GetIO().Framerate);
    ImGui::TextDisabled("built " __DATE__ " " __TIME__);
  }
  ImGui::End();
}
} // namespace

void configure(bool enabled, int toggle_key) {
  g_visible = enabled;
  g_toggle_key = toggle_key;
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
