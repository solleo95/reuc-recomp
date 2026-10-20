#include "dlss5_pass.h"
#include "dlss5_math.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace uc {
namespace {
void check(VkResult result, const char* what) { if (result != VK_SUCCESS) throw std::runtime_error(std::string("DLSS 5: ") + what + " failed: VkResult " + std::to_string(result)); }
// shaders/dlss5_bridge.slang Constants (push constants)
struct Constants { int32_t source_size[2], target_size[2], source_origin[2], target_origin[2]; float gain; int32_t encoding, filter, domain; };
static_assert(sizeof(Constants)==48);
constexpr VkFormat kBridgeFormat=VK_FORMAT_R16G16B16A16_SFLOAT;
void image_barrier(VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask=barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask=barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout=before; barrier.newLayout=after;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.image=image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.imageMemoryBarrierCount=1; info.pImageMemoryBarriers=&barrier;
    vkCmdPipelineBarrier2(command,&info);
}
void memory_barrier(VkCommandBuffer command) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask=barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask=barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.memoryBarrierCount=1; info.pMemoryBarriers=&barrier;
    vkCmdPipelineBarrier2(command,&info);
}
VkShaderModule load_shader(VkDevice device, const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(file)), {});
    if (code.empty() || code.size() % 4) throw std::runtime_error("DLSS 5: shader " + path.filename().string() + " missing or invalid");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize=code.size(); info.pCode=reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module=VK_NULL_HANDLE; check(vkCreateShaderModule(device,&info,nullptr,&module),"shader module");
    return module;
}
NgxImage ngx_image(const UpscaleImage& image, VkExtent2D extent, bool read_write, VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT) {
    return {image.image,image.view,image.format,extent,read_write,aspect};
}
}
void Dlss5Pass::fail(const std::string& reason) {
    failed_=true; ran_=false;
    if (status_!=reason) std::fprintf(stderr,"DLSS 5 unavailable: %s\n",reason.c_str());
    status_=reason;
}
void Dlss5Pass::ensure_pipelines() {
    if (prepare_) return;
    const auto device=context_.device;
    prepare_module_=load_shader(device,context_.shaders/"dlss5.prepare.spv"); resolve_module_=load_shader(device,context_.shaders/"dlss5.resolve.spv");
    const VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount=2; set.pBindings=bindings;
    check(vkCreateDescriptorSetLayout(device,&set,nullptr,&set_layout_),"descriptor layout");
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Constants)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount=1; layout.pSetLayouts=&set_layout_; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
    check(vkCreatePipelineLayout(device,&layout,nullptr,&layout_),"pipeline layout");
    for (auto [module,target]:{std::pair{prepare_module_,&prepare_},std::pair{resolve_module_,&resolve_}}) {
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr}; pipeline.layout=layout_;
        check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline,nullptr,target),"pipeline");
    }
    const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,3},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=3; pool.poolSizeCount=2; pool.pPoolSizes=sizes;
    check(vkCreateDescriptorPool(device,&pool,nullptr,&pool_),"descriptor pool");
    const VkDescriptorSetLayout layouts[3]={set_layout_,set_layout_,set_layout_};
    VkDescriptorSet sets[3]{};
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=pool_; allocate.descriptorSetCount=3; allocate.pSetLayouts=layouts;
    check(vkAllocateDescriptorSets(device,&allocate,sets),"descriptor sets");
    prepare_set_=sets[0]; resolve_from_[0]=sets[1]; resolve_from_[1]=sets[2];
}
Dlss5Pass::Image Dlss5Pass::make_image(VkExtent2D extent) {
    Image result;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=kBridgeFormat;
    info.extent={extent.width,extent.height,1}; info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL;
    info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT; info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(context_.device,&info,nullptr,&result.image),"bridge image");
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(context_.device,result.image,&requirements);
    uint32_t type=UINT32_MAX;
    for (uint32_t i=0;i<context_.memory.memoryTypeCount && type==UINT32_MAX;++i)
        if ((requirements.memoryTypeBits&(1u<<i)) && (context_.memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type=i;
    if (type==UINT32_MAX) throw std::runtime_error("DLSS 5: no device-local memory");
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size; allocate.memoryTypeIndex=type;
    check(vkAllocateMemory(context_.device,&allocate,nullptr,&result.memory),"bridge memory");
    check(vkBindImageMemory(context_.device,result.image,result.memory,0),"bind bridge image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=result.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=kBridgeFormat; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    check(vkCreateImageView(context_.device,&view,nullptr,&result.view),"bridge view");
    return result;
}
void Dlss5Pass::destroy(Image& image) {
    if (image.view) vkDestroyImageView(context_.device,image.view,nullptr);
    if (image.image) vkDestroyImage(context_.device,image.image,nullptr);
    if (image.memory) vkFreeMemory(context_.device,image.memory,nullptr);
    image={};
}
void Dlss5Pass::ensure_images(VkExtent2D extent) {
    if (bridge_[0].image && bridge_extent_.width==extent.width && bridge_extent_.height==extent.height) return;
    if (bridge_[0].image && context_.idle) context_.idle();   // a frame in flight may still use the old ones
    for (auto& image:bridge_) destroy(image);
    for (auto& image:bridge_) image=make_image(extent);
    bridge_extent_=extent; bound_picture_=VK_NULL_HANDLE; bridge_ready_=false;   // the sets point at the old views
}
// The three descriptor sets: picture -> A, A -> picture, B -> picture. They are rewritten only when the picture or the bridge images changed (frames may still use them: idle first).
void Dlss5Pass::bind(VkImageView picture, uint64_t generation) {
    if (bound_picture_==picture && bound_generation_==generation) return;
    if (bound_picture_ && context_.idle) context_.idle();
    const VkDescriptorImageInfo picture_in{VK_NULL_HANDLE,picture,VK_IMAGE_LAYOUT_GENERAL}, a{VK_NULL_HANDLE,bridge_[0].view,VK_IMAGE_LAYOUT_GENERAL}, b{VK_NULL_HANDLE,bridge_[1].view,VK_IMAGE_LAYOUT_GENERAL};
    struct Use { VkDescriptorSet set; const VkDescriptorImageInfo *read, *write; };
    const Use uses[]={{prepare_set_,&picture_in,&a},{resolve_from_[0],&a,&picture_in},{resolve_from_[1],&b,&picture_in}};
    VkWriteDescriptorSet writes[6]{};
    for (size_t i=0;i<3;++i) {
        writes[i*2]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,uses[i].set,0,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,uses[i].read};
        writes[i*2+1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,uses[i].set,1,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,uses[i].write};
    }
    vkUpdateDescriptorSets(context_.device,6,writes,0,nullptr);
    bound_picture_=picture; bound_generation_=generation;
}
bool Dlss5Pass::record(const Dlss5Frame& frame) {
    ran_=false;
    if (open_failed_ || !context_.device) return false;
    try {
        if (!runtime_) {
            NgxInit init; init.instance=context_.instance; init.physical=context_.physical; init.device=context_.device;
            init.instance_proc=context_.instance_proc; init.device_proc=context_.device_proc;
            init.runtime_directory=context_.directory; init.log_file=context_.directory/"debug"/"ngx_dlss5.log"; init.verbose=frame.settings.debug;
            std::string reason;
            runtime_=Dlss5Runtime::open(context_.backend ? context_.backend() : make_ngx_backend(),init,reason);
            if (!runtime_) { open_failed_=true; fail(reason); return false; }
            std::fprintf(stderr,"DLSS 5: NGX session open (log: debug/ngx_dlss5.log)\n");
        }
        if (failed_) return false;
        const auto& s=frame.settings;
        const auto processing=dlss5_processing_extent(frame.size.width,frame.size.height,s.processing_scale);
        if (!runtime_->prepare(frame.command,Dlss5FeatureKey{processing.width,processing.height,s.preset},s.passes,context_.idle)) { fail(runtime_->reason()); return false; }
        ensure_pipelines();
        ensure_images({processing.width,processing.height});
        bind(frame.picture.view,frame.targets_generation);
        if (!bridge_ready_) { for (auto& image:bridge_) image_barrier(frame.command,image.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL); bridge_ready_=true; }   // new images start undefined, then stay in GENERAL
        Constants constants{};
        constants.gain=float(dlss5_gain(s)); constants.encoding=s.signal_encoding==Dlss5Encoding::automatic ? 0 : int(s.signal_encoding)-1;
        constants.domain=int(s.scaling_domain);
        // prepare: picture region -> A
        constants.source_size[0]=int32_t(frame.size.width); constants.source_size[1]=int32_t(frame.size.height);
        constants.target_size[0]=int32_t(processing.width); constants.target_size[1]=int32_t(processing.height);
        constants.source_origin[0]=frame.origin.x; constants.source_origin[1]=frame.origin.y;
        constants.filter=int(s.downsample_filter);
        vkCmdBindPipeline(frame.command,VK_PIPELINE_BIND_POINT_COMPUTE,prepare_);
        vkCmdBindDescriptorSets(frame.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&prepare_set_,0,nullptr);
        vkCmdPushConstants(frame.command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof constants,&constants);
        vkCmdDispatch(frame.command,(processing.width+7)/8,(processing.height+7)/8,1);
        memory_barrier(frame.command);
        // the network: A -> B -> A ... one feature (and history) per pass
        Dlss5Run run;
        run.depth=ngx_image(frame.depth,frame.depth.extent,false,VK_IMAGE_ASPECT_DEPTH_BIT); run.motion=ngx_image(frame.motion,frame.motion.extent,false);
        run.depth_region=run.motion_region=frame.scene_size;
        run.color_region=run.output_region={processing.width,processing.height};
        const float to_processing[2]={float(processing.width)/float(frame.scene_size.width),float(processing.height)/float(frame.scene_size.height)};
        run.motion_scale[0]=to_processing[0]; run.motion_scale[1]=to_processing[1];
        run.jitter[0]=frame.jitter[0]*to_processing[0]; run.jitter[1]=frame.jitter[1]*to_processing[1];
        run.reset=frame.reset; run.settings=s;
        const auto bridge_image=[&](size_t i, bool write) { return NgxImage{bridge_[i].image,bridge_[i].view,kBridgeFormat,bridge_extent_,write,VK_IMAGE_ASPECT_COLOR_BIT}; };
        const uint32_t passes=runtime_->passes();
        for (uint32_t pass=0;pass<passes;++pass) {
            run.color=bridge_image(pass&1,false); run.output=bridge_image((pass&1)^1,true);
            if (!runtime_->evaluate(frame.command,pass,run)) { fail(runtime_->reason()); return false; }
            memory_barrier(frame.command);
        }
        // resolve: the last output -> picture region
        constants.source_size[0]=int32_t(processing.width); constants.source_size[1]=int32_t(processing.height);
        constants.target_size[0]=int32_t(frame.size.width); constants.target_size[1]=int32_t(frame.size.height);
        constants.source_origin[0]=constants.source_origin[1]=0; constants.target_origin[0]=frame.origin.x; constants.target_origin[1]=frame.origin.y;
        constants.filter=int(s.upsample_filter);
        const size_t last=passes&1;   // after an odd number of passes the result is in B
        vkCmdBindPipeline(frame.command,VK_PIPELINE_BIND_POINT_COMPUTE,resolve_);
        vkCmdBindDescriptorSets(frame.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&resolve_from_[last],0,nullptr);
        vkCmdPushConstants(frame.command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof constants,&constants);
        vkCmdDispatch(frame.command,(frame.size.width+7)/8,(frame.size.height+7)/8,1);
        memory_barrier(frame.command);
        status_.clear(); ran_=true;
        if (s.debug && frames_%240==0) std::fprintf(stderr,"DLSS 5: %ux%u -> %ux%u, %u pass(es), stage %u, filters %u/%u\n",frame.size.width,frame.size.height,processing.width,processing.height,passes,unsigned(s.stage),unsigned(s.downsample_filter),unsigned(s.upsample_filter));
        ++frames_;
        return true;
    } catch (const std::exception& error) {
        fail(error.what());
        return false;
    }
}
void Dlss5Pass::shutdown() {
    if (!context_.device) return;
    if (context_.idle) context_.idle();
    runtime_.reset();   // the features and the NGX session go before the device
    for (auto& image:bridge_) destroy(image);
    const auto device=context_.device;
    if (pool_) vkDestroyDescriptorPool(device,pool_,nullptr);
    if (prepare_) vkDestroyPipeline(device,prepare_,nullptr);
    if (resolve_) vkDestroyPipeline(device,resolve_,nullptr);
    if (layout_) vkDestroyPipelineLayout(device,layout_,nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device,set_layout_,nullptr);
    if (prepare_module_) vkDestroyShaderModule(device,prepare_module_,nullptr);
    if (resolve_module_) vkDestroyShaderModule(device,resolve_module_,nullptr);
    pool_=VK_NULL_HANDLE; prepare_=resolve_=VK_NULL_HANDLE; layout_=VK_NULL_HANDLE; set_layout_=VK_NULL_HANDLE; prepare_module_=resolve_module_=VK_NULL_HANDLE;
    bridge_extent_={}; bound_picture_=VK_NULL_HANDLE; bridge_ready_=false; context_.device=VK_NULL_HANDLE;
}
}
