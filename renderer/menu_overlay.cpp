#include "menu_overlay.h"
#include <intrin.h>
#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include <stdexcept>
#include <string>
namespace uc {
namespace {
struct Context {
    ImGuiContext* previous=ImGui::GetCurrentContext();
    explicit Context(ImGuiContext* current){ImGui::SetCurrentContext(current);}
    ~Context(){ImGui::SetCurrentContext(previous);}
};
void check(VkResult result) {if(result!=VK_SUCCESS)throw std::runtime_error("menu overlay Vulkan error: "+std::to_string(result));}
}
void MenuOverlay::initialize(SDL_Window* window,VkInstance instance,VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family,uint32_t slots,const std::function<void()>& setup) {
    if(context_)return;
    context_=ImGui::CreateContext();Context current(context_);
    try {
        auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
        io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        if(setup)setup();
        if(!ImGui_ImplSDL3_InitForVulkan(window))throw std::runtime_error("menu overlay SDL initialization failed");
        ImGui_ImplVulkan_InitInfo info{};info.ApiVersion=VK_API_VERSION_1_3;info.Instance=instance;info.PhysicalDevice=physical;info.Device=device;
        info.QueueFamily=family;info.Queue=queue;info.DescriptorPoolSize=32;info.MinImageCount=2;info.ImageCount=slots;info.UseDynamicRendering=true;info.CheckVkResultFn=check;
        auto& render=info.PipelineInfoMain.PipelineRenderingCreateInfo;
        render={VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};render.colorAttachmentCount=1;render.pColorAttachmentFormats=&format_;
        if(!ImGui_ImplVulkan_Init(&info))throw std::runtime_error("menu overlay Vulkan initialization failed");
    } catch(...) {shutdown();throw;}
}
bool MenuOverlay::event(const SDL_Event& event) {
    if(!context_)return false;
    Context current(context_);
    ImGui_ImplSDL3_ProcessEvent(&event);
    const auto& io=ImGui::GetIO();
    return io.WantCaptureMouse || io.WantCaptureKeyboard || io.WantTextInput;
}
void MenuOverlay::record(VkCommandBuffer command,VkExtent2D extent,const std::function<void()>& build) {
    if(!context_)throw std::logic_error("menu overlay not initialized");
    Context current(context_);
    ImGui_ImplVulkan_NewFrame();ImGui_ImplSDL3_NewFrame();
    // The frame image, not the window: it can lag a resize by the rebuild debounce. shortcut: assumes window units are pixels (the game window has no high-density flag).
    auto& io=ImGui::GetIO();io.DisplaySize={float(extent.width),float(extent.height)};io.DisplayFramebufferScale={1,1};
    ImGui::NewFrame();
    build();
    ImGui::Render();ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),command);
}
void MenuOverlay::shutdown() {
    if(!context_)return;
    auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(context_);
    if(ImGui::GetIO().BackendRendererUserData)ImGui_ImplVulkan_Shutdown();
    if(ImGui::GetIO().BackendPlatformUserData)ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(context_);ImGui::SetCurrentContext(previous==context_ ? nullptr : previous);
    context_=nullptr;
}
}
