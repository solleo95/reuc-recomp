#pragma once
#include "prompt_draw.h"
#include <vulkan/vulkan.h>
#include <span>
struct ImGuiContext;
namespace uc {
class PromptOverlay {
public:
    void initialize(VkInstance,VkPhysicalDevice,VkDevice,VkQueue,uint32_t family,uint32_t slots);
    void record(VkCommandBuffer,std::span<const gx::PromptDraw>,VkExtent2D,const std::array<uint32_t,4>& source);
    void shutdown(); // Renderer waits for its GPU work before calling this.
private:
    ImGuiContext* context_=nullptr;
    std::vector<char> font_bytes_;
    VkFormat format_=VK_FORMAT_R8G8B8A8_UNORM;
};
}
