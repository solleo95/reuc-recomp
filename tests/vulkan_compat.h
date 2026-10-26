#pragma once
#include <cstring>
#include <vulkan/vulkan_win32.h>

// Hide capabilities at the Vulkan boundary; all rendering still uses the real driver.
enum class MissingMaintenance { none,surface,device,feature };
static MissingMaintenance missing_maintenance=MissingMaintenance::none;
static bool missing_unused_attachments=false;
static bool missing_exclusive_policy=false,expect_exclusive_fullscreen=false;
static bool exclusive_policy_available=false,surface_capabilities2_available=false;
template<class Function> Function vulkan_export(const char* name) {
    auto module=GetModuleHandleW(L"vulkan-1.dll");
    auto address=module ? GetProcAddress(module,name) : nullptr;
    CHECK(address);
    return reinterpret_cast<Function>(address);
}
template<class Enumerate> VkResult filtered_extensions(Enumerate enumerate,uint32_t* count,VkExtensionProperties* output,bool hide,bool hide_unused=false,bool hide_exclusive=false) {
    if(!hide && !hide_unused && !hide_exclusive)return enumerate(count,output);
    uint32_t size=0;
    auto result=enumerate(&size,nullptr);
    if(result!=VK_SUCCESS)return result;
    std::vector<VkExtensionProperties> extensions(size);
    result=enumerate(&size,extensions.data());
    if(result!=VK_SUCCESS)return result;
    extensions.resize(size);
    std::erase_if(extensions,[&](const auto& item){
        return (hide && std::strstr(item.extensionName,"maintenance1")!=nullptr) ||
            (hide_unused && std::strcmp(item.extensionName,VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME)==0) ||
            (hide_exclusive && std::strcmp(item.extensionName,VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME)==0);
    });
    if(!output) {*count=uint32_t(extensions.size());return VK_SUCCESS;}
    const auto copied=std::min(*count,uint32_t(extensions.size()));
    std::copy_n(extensions.begin(),copied,output);*count=copied;
    return copied<extensions.size() ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* layer,uint32_t* count,VkExtensionProperties* output) {
    auto enumerate=vulkan_export<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties");
    const auto result=filtered_extensions([&](uint32_t* n,VkExtensionProperties* p){return enumerate(layer,n,p);},count,output,missing_maintenance==MissingMaintenance::surface);
    if(!layer && output && result==VK_SUCCESS)surface_capabilities2_available=std::any_of(output,output+*count,[](const auto& extension){return std::strcmp(extension.extensionName,VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)==0;});
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice device,const char* layer,uint32_t* count,VkExtensionProperties* output) {
    auto enumerate=vulkan_export<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    const auto result=filtered_extensions([&](uint32_t* n,VkExtensionProperties* p){return enumerate(device,layer,n,p);},count,output,missing_maintenance==MissingMaintenance::device,missing_unused_attachments,missing_exclusive_policy);
    // Initialization queries the selected adapter again before creating its device.
    if(!layer && output && result==VK_SUCCESS)exclusive_policy_available=std::any_of(output,output+*count,[](const auto& extension){return std::strcmp(extension.extensionName,VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME)==0;});
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device,const VkSwapchainCreateInfoKHR* info,const VkAllocationCallbacks* allocator,VkSwapchainKHR* swapchain) {
    const VkSurfaceFullScreenExclusiveInfoEXT* policy=nullptr;
    for(auto* item=static_cast<const VkBaseInStructure*>(info->pNext);item;item=item->pNext)
        if(item->sType==VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT)policy=reinterpret_cast<const VkSurfaceFullScreenExclusiveInfoEXT*>(item);
    CHECK(bool(policy)==(exclusive_policy_available && surface_capabilities2_available && !expect_exclusive_fullscreen));
    if(policy)CHECK(policy->fullScreenExclusive==VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT);
    return vulkan_export<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(device,info,allocator,swapchain);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice device,VkPhysicalDeviceFeatures2* features) {
    vulkan_export<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(device,features);
    if(missing_maintenance==MissingMaintenance::feature)
        for(auto* item=static_cast<VkBaseOutStructure*>(features->pNext);item;item=item->pNext)
            if(item->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR)
                reinterpret_cast<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR*>(item)->swapchainMaintenance1=VK_FALSE;
    if(missing_unused_attachments)
        for(auto* item=static_cast<VkBaseOutStructure*>(features->pNext);item;item=item->pNext)
            if(item->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT)
                reinterpret_cast<VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT*>(item)->dynamicRenderingUnusedAttachments=VK_FALSE;
}
