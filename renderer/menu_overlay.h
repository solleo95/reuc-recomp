#pragma once
#include <vulkan/vulkan.h>
#include <functional>
struct ImGuiContext;
struct SDL_Window;
union SDL_Event;
namespace uc {
// The in-game menu: an ImGui context of its own with the SDL3 platform and Vulkan renderer backends, drawn over the presented frame.
class MenuOverlay {
public:
    // setup runs once with the new context current (fonts, style); the caller keeps any font memory alive while the overlay exists.
    void initialize(SDL_Window*,VkInstance,VkPhysicalDevice,VkDevice,VkQueue,uint32_t family,uint32_t slots,const std::function<void()>& setup);
    bool initialized() const { return context_!=nullptr; }
    bool event(const SDL_Event&);   // true when the menu wants the event (mouse over a window, text input)
    void record(VkCommandBuffer,VkExtent2D,const std::function<void()>& build);   // inside a dynamic rendering pass on the frame image
    void shutdown(); // Renderer waits for its GPU work before calling this.
private:
    ImGuiContext* context_=nullptr;
    VkFormat format_=VK_FORMAT_R8G8B8A8_UNORM;
};
}
