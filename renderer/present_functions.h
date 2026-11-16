#pragma once
#include <vulkan/vulkan.h>
#include <functional>

namespace uc {
// Swapchain and presentation calls of the renderer. Frame generation replaces them: Streamline's proxies for DLSS-G, the FFX frame
// interpolation swapchain's functions for FSR FG. Everything else calls Vulkan directly, looked up at the call: with volk (Android) the vk*
// names are pointers filled once the driver loads, after this object is made.
struct PresentFunctions {
    std::function<VkResult(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*)> create=
        [](VkDevice d, const VkSwapchainCreateInfoKHR* i, const VkAllocationCallbacks* a, VkSwapchainKHR* s) { return vkCreateSwapchainKHR(d, i, a, s); };
    std::function<void(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*)> destroy=
        [](VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks* a) { vkDestroySwapchainKHR(d, s, a); };
    std::function<VkResult(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*)> images=
        [](VkDevice d, VkSwapchainKHR s, uint32_t* n, VkImage* i) { return vkGetSwapchainImagesKHR(d, s, n, i); };
    std::function<VkResult(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*)> acquire=
        [](VkDevice d, VkSwapchainKHR s, uint64_t t, VkSemaphore m, VkFence f, uint32_t* i) { return vkAcquireNextImageKHR(d, s, t, m, f, i); };
    std::function<VkResult(VkQueue, const VkPresentInfoKHR*)> present=[](VkQueue q, const VkPresentInfoKHR* i) { return vkQueuePresentKHR(q, i); };
    std::function<VkResult(VkDevice)> wait_idle=[](VkDevice d) { return vkDeviceWaitIdle(d); };
};
}
