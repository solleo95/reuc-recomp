#pragma once
#include <vulkan/vulkan.h>
#include <functional>

namespace uc {
// Swapchain and presentation calls of the renderer. Frame generation replaces them: Streamline's proxies for DLSS-G, the FFX frame
// interpolation swapchain's functions for FSR FG. Everything else calls the Vulkan loader directly.
struct PresentFunctions {
    std::function<VkResult(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*)> create=vkCreateSwapchainKHR;
    std::function<void(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*)> destroy=vkDestroySwapchainKHR;
    std::function<VkResult(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*)> images=vkGetSwapchainImagesKHR;
    std::function<VkResult(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*)> acquire=vkAcquireNextImageKHR;
    std::function<VkResult(VkQueue, const VkPresentInfoKHR*)> present=vkQueuePresentKHR;
    std::function<VkResult(VkDevice)> wait_idle=vkDeviceWaitIdle;
};
}
