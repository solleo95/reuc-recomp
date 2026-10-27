#include "prompt_overlay.h"
#include "prompt_picture.h"
#include "imgui.h"
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
void check(VkResult result) {if(result!=VK_SUCCESS)throw std::runtime_error("native prompt Vulkan error: "+std::to_string(result));}
}
void PromptOverlay::initialize(VkInstance instance,VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family,uint32_t slots) {
    if(context_)return;
    context_=ImGui::CreateContext();Context current(context_);
    try {
        auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
        load_prompt_font(font_bytes_);
        ImGui_ImplVulkan_InitInfo info{};info.ApiVersion=VK_API_VERSION_1_3;info.Instance=instance;info.PhysicalDevice=physical;info.Device=device;
        info.QueueFamily=family;info.Queue=queue;info.DescriptorPoolSize=32;info.MinImageCount=2;info.ImageCount=slots;info.UseDynamicRendering=true;info.CheckVkResultFn=check;
        auto& render=info.PipelineInfoMain.PipelineRenderingCreateInfo;
        render={VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};render.colorAttachmentCount=1;render.pColorAttachmentFormats=&format_;
        if(!ImGui_ImplVulkan_Init(&info))throw std::runtime_error("native prompt Vulkan initialization failed");
    } catch(...) {shutdown();throw;}
}
void PromptOverlay::record(VkCommandBuffer command,std::span<const gx::PromptDraw> prompts,VkExtent2D extent,const std::array<uint32_t,4>& source,PromptStyle style) {
    if(!context_ || !source[2] || !source[3])throw std::logic_error("native prompt renderer not initialized");
    Context current(context_);auto& io=ImGui::GetIO();io.DisplaySize={float(extent.width),float(extent.height)};io.DeltaTime=1.f/60;
    ImGui_ImplVulkan_NewFrame();ImGui::NewFrame();
    for(const auto& prompt:prompts)draw_prompt_picture(prompt,{extent.width,extent.height},source,style);
    ImGui::Render();ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),command);
}
void PromptOverlay::shutdown() {
    if(!context_)return;
    auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(context_);
    if(ImGui::GetIO().BackendRendererUserData)ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext(context_);ImGui::SetCurrentContext(previous==context_ ? nullptr : previous);
    context_=nullptr;font_bytes_.clear();
}
}
