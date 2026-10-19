#include "shadow_map.h"
#include <stdexcept>
namespace uc {
void ShadowMap::ensure(uint32_t resolution) {
    if (resolution==resolution_ && image_) return;
    shutdown();
    if (!resolution) return;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=VK_FORMAT_D32_SFLOAT;
    info.extent={resolution,resolution,1}; info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL;
    info.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT; info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_,&info,nullptr,&image_)!=VK_SUCCESS) throw std::runtime_error("shadow map image");
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device_,image_,&requirements);
    uint32_t type=UINT32_MAX;
    for (uint32_t i=0;i<memory_.memoryTypeCount && type==UINT32_MAX;++i)
        if ((requirements.memoryTypeBits&(1u<<i)) && (memory_.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type=i;
    if (type==UINT32_MAX) throw std::runtime_error("shadow map: no device-local memory");
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size; allocate.memoryTypeIndex=type;
    if (vkAllocateMemory(device_,&allocate,nullptr,&memory_object_)!=VK_SUCCESS || vkBindImageMemory(device_,image_,memory_object_,0)!=VK_SUCCESS) throw std::runtime_error("shadow map memory");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=image_; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=VK_FORMAT_D32_SFLOAT; view.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    if (vkCreateImageView(device_,&view,nullptr,&view_)!=VK_SUCCESS) throw std::runtime_error("shadow map view");
    resolution_=resolution; ++generation_;
}
void ShadowMap::shutdown() {
    if (!device_) return;
    if (view_) vkDestroyImageView(device_,view_,nullptr);
    if (image_) vkDestroyImage(device_,image_,nullptr);
    if (memory_object_) vkFreeMemory(device_,memory_object_,nullptr);
    view_=VK_NULL_HANDLE; image_=VK_NULL_HANDLE; memory_object_=VK_NULL_HANDLE; resolution_=0;
}
}
