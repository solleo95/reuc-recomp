#include "rr_inputs_pass.h"
#include <algorithm>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace uc {
namespace {
void check(VkResult result, const char* what) { if (result != VK_SUCCESS) throw std::runtime_error(std::string("rr inputs: ") + what + " failed: VkResult " + std::to_string(result)); }
// shaders/rr_inputs.slang Params (push constants)
struct PushParams { float viewport[4], view_a[4], view_b[2], depth_range[2]; uint32_t region[2], padding[2]; };
static_assert(sizeof(PushParams)==64 && offsetof(PushParams,region)==48);
constexpr uint32_t kBindings=6;
void barrier_all(VkCommandBuffer command) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask=barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; barrier.srcAccessMask=barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.memoryBarrierCount=1; info.pMemoryBarriers=&barrier;
    vkCmdPipelineBarrier2(command,&info);
}
}
void RrInputsPass::initialize(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, uint32_t slots, std::function<void()> idle) {
    if (device_) return;
    if (!slots || slots>sets_.size()) throw std::invalid_argument("rr inputs: unsupported frame slot count");
    device_=device; memory_=memory; slots_=slots;
    idle_=idle ? std::move(idle) : [device] { vkDeviceWaitIdle(device); };
    try {
        std::ifstream file(shaders/"rr_inputs.spv",std::ios::binary);
        std::vector<char> code((std::istreambuf_iterator<char>(file)),{});
        if (code.empty() || code.size()%4) throw std::runtime_error("rr inputs: shader rr_inputs.spv missing or invalid");
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize=code.size(); module.pCode=reinterpret_cast<const uint32_t*>(code.data());
        check(vkCreateShaderModule(device_,&module,nullptr,&module_),"shader module");
        const VkDescriptorType types[kBindings]={VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        VkDescriptorSetLayoutBinding bindings[kBindings]{};
        for (uint32_t i=0;i<kBindings;++i) bindings[i]={i,types[i],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount=kBindings; set.pBindings=bindings;
        check(vkCreateDescriptorSetLayout(device_,&set,nullptr,&set_layout_),"descriptor layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(PushParams)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount=1; layout.pSetLayouts=&set_layout_; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device_,&layout,nullptr,&layout_),"pipeline layout");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module_,"inputsMain",nullptr};
        pipeline.layout=layout_;
        check(vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipeline_),"pipeline");
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,3*slots},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,slots},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2*slots}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=slots; pool.poolSizeCount=3; pool.pPoolSizes=sizes;
        check(vkCreateDescriptorPool(device_,&pool,nullptr,&pool_),"descriptor pool");
        const std::vector<VkDescriptorSetLayout> layouts(slots,set_layout_);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=pool_; allocate.descriptorSetCount=slots; allocate.pSetLayouts=layouts.data();
        check(vkAllocateDescriptorSets(device_,&allocate,sets_.data()),"descriptor sets");
    } catch (...) { shutdown(); throw; }
}
RrInputsPass::Image RrInputsPass::make_image(VkExtent2D extent, VkFormat format) {
    Image image;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=format;
    info.extent={extent.width,extent.height,1}; info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
    info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=kUsage; info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_,&info,nullptr,&image.image),"image");
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device_,image.image,&requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size; allocate.memoryTypeIndex=UINT32_MAX;
    for (uint32_t i=0;i<memory_.memoryTypeCount;++i) if ((requirements.memoryTypeBits&(1u<<i)) && (memory_.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { allocate.memoryTypeIndex=i; break; }
    if (allocate.memoryTypeIndex==UINT32_MAX) throw std::runtime_error("rr inputs: required memory type unavailable");
    check(vkAllocateMemory(device_,&allocate,nullptr,&image.memory),"memory");
    check(vkBindImageMemory(device_,image.image,image.memory,0),"bind image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=image.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=format; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    check(vkCreateImageView(device_,&view,nullptr,&image.view),"view");
    return image;
}
void RrInputsPass::destroy_image(Image& image) {
    if (image.view) vkDestroyImageView(device_,image.view,nullptr);
    if (image.image) vkDestroyImage(device_,image.image,nullptr);
    if (image.memory) vkFreeMemory(device_,image.memory,nullptr);
    image={};
}
void RrInputsPass::ensure(VkExtent2D extent) {
    if (normal_.image && extent.width==extent_.width && extent.height==extent_.height) return;
    if (normal_.image) { idle_(); destroy_image(normal_); destroy_image(specular_); }   // frames in flight may still read the images (Streamline's evaluation)
    normal_=make_image(extent,VK_FORMAT_R16G16B16A16_SFLOAT); specular_=make_image(extent,VK_FORMAT_R8G8B8A8_UNORM);
    extent_=extent; initialized_=false;
}
void RrInputsPass::record(const RrInputs& in) {
    if (in.slot>=slots_) throw std::invalid_argument("rr inputs: frame slot out of range");
    ensure(in.extent);
    if (!initialized_) {   // new images to VK_IMAGE_LAYOUT_GENERAL
        VkImageMemoryBarrier2 barriers[2]{};
        const VkImage images[2]={normal_.image,specular_.image};
        for (int i=0;i<2;++i) {
            barriers[i].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barriers[i].srcStageMask=barriers[i].dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; barriers[i].srcAccessMask=barriers[i].dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
            barriers[i].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barriers[i].newLayout=VK_IMAGE_LAYOUT_GENERAL; barriers[i].srcQueueFamilyIndex=barriers[i].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barriers[i].image=images[i]; barriers[i].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        }
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount=2; dependency.pImageMemoryBarriers=barriers;
        vkCmdPipelineBarrier2(in.command,&dependency);
        initialized_=true;
    }
    barrier_all(in.command);   // the previous frame's evaluation may still read the images
    const VkDescriptorImageInfo images[5]={{VK_NULL_HANDLE,in.depth,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,in.gbuffer,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,in.material,VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE,normal_.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,specular_.view,VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo records{in.records,0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[kBindings]{};
    for (uint32_t i=0;i<kBindings;++i) { writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=sets_[in.slot]; writes[i].dstBinding=i; writes[i].descriptorCount=1; }
    for (uint32_t i=0;i<3;++i) { writes[i].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[i].pImageInfo=&images[i]; }
    writes[3].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[3].pBufferInfo=&records;
    for (uint32_t i=4;i<6;++i) { writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[i].pImageInfo=&images[i-1]; }
    vkUpdateDescriptorSets(device_,kBindings,writes,0,nullptr);   // the slot's previous frame has completed
    PushParams params{};
    std::copy(in.viewport.begin(),in.viewport.begin()+4,params.viewport);
    params.view_a[0]=in.view.x_scale; params.view_a[1]=in.view.y_scale; params.view_a[2]=in.view.x_offset; params.view_a[3]=in.view.y_offset;
    params.view_b[0]=in.view.depth_a; params.view_b[1]=in.view.depth_b;
    params.depth_range[0]=in.viewport[4]; params.depth_range[1]=in.viewport[5];
    params.region[0]=in.region.width; params.region[1]=in.region.height;
    vkCmdBindPipeline(in.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
    vkCmdBindDescriptorSets(in.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&sets_[in.slot],0,nullptr);
    vkCmdPushConstants(in.command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,&params);
    vkCmdDispatch(in.command,(in.region.width+7)/8,(in.region.height+7)/8,1);
    barrier_all(in.command);
}
void RrInputsPass::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    destroy_image(normal_); destroy_image(specular_);
    if (pool_) vkDestroyDescriptorPool(device_,pool_,nullptr);
    if (pipeline_) vkDestroyPipeline(device_,pipeline_,nullptr);
    if (layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_,set_layout_,nullptr);
    if (module_) vkDestroyShaderModule(device_,module_,nullptr);
    *this=RrInputsPass{};
}
}
