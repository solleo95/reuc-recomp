#include "check.h"
#include "buffer_memory.h"
#include <vector>

int main() {
    VkPhysicalDeviceMemoryProperties memory{};
    memory.memoryTypeCount=3;
    memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    memory.memoryTypes[1].propertyFlags=memory.memoryTypes[0].propertyFlags|VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    memory.memoryTypes[2].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    std::vector<uint32_t> attempts;
    auto allocate=[&](uint32_t type) { attempts.push_back(type); return type==1 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS; };
    CHECK(uc::allocate_host_buffer(memory,7,true,allocate)==VK_SUCCESS);
    CHECK((attempts==std::vector<uint32_t>{1,0}));
    attempts.clear();
    CHECK(uc::allocate_host_buffer(memory,7,false,allocate)==VK_SUCCESS);
    CHECK((attempts==std::vector<uint32_t>{0}));
    attempts.clear();
    CHECK(uc::allocate_host_buffer(memory,2,true,allocate)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK((attempts==std::vector<uint32_t>{1}));
    attempts.clear();
    CHECK(uc::allocate_host_buffer(memory,4,true,allocate)==VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(attempts.empty());
    CHECK(uc::allocate_host_buffer(memory,7,true,[&](uint32_t type) { attempts.push_back(type); return VK_ERROR_DEVICE_LOST; })==VK_ERROR_DEVICE_LOST);
    CHECK((attempts==std::vector<uint32_t>{1}));
}
