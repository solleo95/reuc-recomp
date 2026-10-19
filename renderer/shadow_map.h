#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
namespace uc {
// The depth image of the shadow map (D32, sampled by the composite). The renderer draws into it inside the scene packets and tracks its layout;
// ensure() recreates it for a new resolution, so the caller must have waited for the GPU.
class ShadowMap {
public:
    void initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory) { device_=device; memory_=memory; }
    void ensure(uint32_t resolution);
    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    uint32_t resolution() const { return resolution_; }
    uint64_t generation() const { return generation_; }   // changes whenever the image is recreated (a new view may reuse a destroyed one's handle)
    void shutdown();
private:
    VkDevice device_=VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkImage image_=VK_NULL_HANDLE; VkDeviceMemory memory_object_=VK_NULL_HANDLE; VkImageView view_=VK_NULL_HANDLE;
    uint32_t resolution_=0;
    uint64_t generation_=0;
};
}
