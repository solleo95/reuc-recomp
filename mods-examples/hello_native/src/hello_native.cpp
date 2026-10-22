// A native mod: its own settings window (gear button in the Mods tab of the Insert menu), drawn with Dear ImGui, reading the game API through JSON.
// Build it as in CMakeLists.txt (a 64-bit DLL with its own copy of the ImGui version the port uses) and put it at native/hello_native.dll.
#include "uc_mod.h"
#include <imgui.h>
#include <cstring>

UC_MOD_DEFINE_INFO()   // the port refuses the DLL when the API or ImGui version it was built with does not match

static const UcApi* g_api;
static int g_clicks;
static float g_value = 0.5f;

static void draw(void*) {
    // At the start of every draw: the port hands over its ImGui context and allocator right before this call (they are not known earlier).
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(g_api->imgui_context));
    ImGui::SetAllocatorFunctions(g_api->imgui_alloc, g_api->imgui_free, g_api->imgui_alloc_user);
    ImGui::TextWrapped("Hello from a native mod.");
    if (ImGui::Button("Click me")) ++g_clicks;
    ImGui::SameLine();
    ImGui::Text("clicked %d times", g_clicks);
    ImGui::SliderFloat("a value", &g_value, 0.f, 1.f);
    const char* result = nullptr;
    if (g_api->get_json("player.present", "{\"slot\":1}", &result) == 0) ImGui::Text("player 1 in the game: %s", result);
}

extern "C" {
UC_MOD_EXPORT int uc_mod_init(const UcApi* api) {
    if (std::strcmp(IMGUI_VERSION, UC_MOD_IMGUI_VERSION) != 0) return 1;   // built with another ImGui than the port's
    g_api = api;
    api->log("hello_native started");
    api->set_ui(draw, nullptr);
    return 0;
}

UC_MOD_EXPORT void uc_mod_shutdown(void) {}
}
