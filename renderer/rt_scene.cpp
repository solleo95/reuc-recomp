#include "rt_scene.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>

namespace uc {
namespace {
void check(VkResult result, const char* what) { if (result!=VK_SUCCESS) throw std::runtime_error(std::string("ray tracing: ")+what+" failed: VkResult "+std::to_string(result)); }
// shaders/rt_transform.slang Push
struct TransformPush { VkDeviceAddress vertices, positions, palettes; uint32_t count; int32_t first_vertex; uint32_t palette, current, indexed; };
static_assert(sizeof(TransformPush)==48);
// shaders/rt_probe.slang Params
struct ProbeParams { float viewport[4]; float view_a[4]; float view_b[2]; float depth_range[2]; float to_light[4]; uint32_t region[2]; uint32_t reserved[2]; };
static_assert(sizeof(ProbeParams)==80);
constexpr VkDeviceSize kCounterStride=256;   // >= minStorageBufferOffsetAlignment everywhere
constexpr VkDeviceSize kMinimumBuffer=VkDeviceSize(1)<<16;
VkDeviceSize align(VkDeviceSize value, VkDeviceSize alignment) { return (value+alignment-1)/alignment*alignment; }
void memory_barrier(VkCommandBuffer command, VkPipelineStageFlags2 source, VkAccessFlags2 source_access, VkPipelineStageFlags2 destination, VkAccessFlags2 destination_access) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask=source; barrier.srcAccessMask=source_access; barrier.dstStageMask=destination; barrier.dstAccessMask=destination_access;
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.memoryBarrierCount=1; info.pMemoryBarriers=&barrier;
    vkCmdPipelineBarrier2(command,&info);
}
VkShaderModule load_module(VkDevice device, const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(file)),{});
    if (code.empty() || code.size()%4) throw std::runtime_error("ray tracing: shader "+path.string()+" missing or invalid");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize=code.size(); info.pCode=reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module; check(vkCreateShaderModule(device,&info,nullptr,&module),"shader module"); return module;
}
constexpr VkBufferUsageFlags kAddress=VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
}
void RtScene::initialize(VkDevice device, VkPhysicalDevice physical, const VkPhysicalDeviceMemoryProperties& memory, const std::filesystem::path& shaders, uint32_t frames_in_flight) {
    if (device_) return;
    device_=device; memory_=memory;
    try {
        const auto load=[&](auto& function, const char* name) {
            function=reinterpret_cast<std::remove_reference_t<decltype(function)>>(vkGetDeviceProcAddr(device,name));
            if (!function) throw std::runtime_error(std::string("ray tracing: ")+name+" unavailable");
        };
        load(create_as_,"vkCreateAccelerationStructureKHR"); load(destroy_as_,"vkDestroyAccelerationStructureKHR");
        load(build_sizes_,"vkGetAccelerationStructureBuildSizesKHR"); load(build_,"vkCmdBuildAccelerationStructuresKHR");
        load(as_address_,"vkGetAccelerationStructureDeviceAddressKHR");
        VkPhysicalDeviceAccelerationStructurePropertiesKHR as_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; properties.pNext=&as_properties;
        vkGetPhysicalDeviceProperties2(physical,&properties);
        scratch_alignment_=std::max<VkDeviceSize>(as_properties.minAccelerationStructureScratchOffsetAlignment,1);
        transform_module_=load_module(device,shaders/"rt_transform.spv"); probe_module_=load_module(device,shaders/"rt_probe.spv");
        {
            const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(TransformPush)};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
            check(vkCreatePipelineLayout(device,&layout,nullptr,&transform_layout_),"transform layout");
        }
        {
            const VkDescriptorSetLayoutBinding bindings[3]{{0,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
                {1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
            VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount=3; set.pBindings=bindings;
            check(vkCreateDescriptorSetLayout(device,&set,nullptr,&probe_set_layout_),"probe set layout");
            const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(ProbeParams)};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount=1; layout.pSetLayouts=&probe_set_layout_; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
            check(vkCreatePipelineLayout(device,&layout,nullptr,&probe_layout_),"probe layout");
        }
        for (auto [module,layout,pipeline] : {std::tuple{transform_module_,transform_layout_,&transform_},std::tuple{probe_module_,probe_layout_,&probe_}}) {
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
            info.layout=layout;
            check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&info,nullptr,pipeline),"pipeline");
        }
        frames_.resize(frames_in_flight);
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,frames_in_flight},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,frames_in_flight},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,frames_in_flight}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=frames_in_flight; pool.poolSizeCount=3; pool.pPoolSizes=sizes;
        check(vkCreateDescriptorPool(device,&pool,nullptr,&pool_),"descriptor pool");
        const std::vector<VkDescriptorSetLayout> layouts(frames_in_flight,probe_set_layout_);
        std::vector<VkDescriptorSet> sets(frames_in_flight);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=pool_; allocate.descriptorSetCount=frames_in_flight; allocate.pSetLayouts=layouts.data();
        check(vkAllocateDescriptorSets(device,&allocate,sets.data()),"descriptor sets");
        for (uint32_t i=0;i<frames_in_flight;++i) frames_[i].probe_set=sets[i];
        counters_=make_buffer(kCounterStride*frames_in_flight,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
    } catch (...) { shutdown(); throw; }
}
RtScene::Buffer RtScene::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    Buffer buffer; buffer.size=size;
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; create.size=size; create.usage=usage;
    check(vkCreateBuffer(device_,&create,nullptr,&buffer.buffer),"buffer");
    try {
        VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device_,buffer.buffer,&requirements);
        const VkMemoryPropertyFlags wanted=host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        uint32_t type=UINT32_MAX;
        for (uint32_t i=0;i<memory_.memoryTypeCount && type==UINT32_MAX;++i) if ((requirements.memoryTypeBits&(1u<<i)) && (memory_.memoryTypes[i].propertyFlags&wanted)==wanted) type=i;
        if (type==UINT32_MAX) throw std::runtime_error("ray tracing: no memory type");
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO}; flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size; allocate.memoryTypeIndex=type;
        if (usage&kAddress) allocate.pNext=&flags;
        check(vkAllocateMemory(device_,&allocate,nullptr,&buffer.memory),"buffer memory");
        check(vkBindBufferMemory(device_,buffer.buffer,buffer.memory,0),"bind buffer");
        if (host) check(vkMapMemory(device_,buffer.memory,0,VK_WHOLE_SIZE,0,&buffer.mapped),"map buffer");
        if (usage&kAddress) { VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; info.buffer=buffer.buffer; buffer.address=vkGetBufferDeviceAddress(device_,&info); }
    } catch (...) { destroy(buffer); throw; }
    ++growths_;
    return buffer;
}
void RtScene::destroy(Buffer& buffer) {
    if (buffer.mapped) vkUnmapMemory(device_,buffer.memory);
    if (buffer.buffer) vkDestroyBuffer(device_,buffer.buffer,nullptr);
    if (buffer.memory) vkFreeMemory(device_,buffer.memory,nullptr);
    buffer={};
}
bool RtScene::ensure(Frame& frame, Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    if (buffer.buffer && buffer.size>=size) return false;
    const VkDeviceSize grown=std::max({size,buffer.size*2,kMinimumBuffer});   // geometric: a frame that keeps growing reallocates O(log n) times
    if (buffer.buffer) frame.retired.push_back(buffer);
    buffer=make_buffer(grown,usage|kAddress,host);
    return true;
}
void RtScene::begin_frame(uint32_t frame_slot) {
    slot_=frame_slot%uint32_t(frames_.size());
    frame_=&frames_[slot_];
    for (auto blas : frame_->blas) destroy_as_(device_,blas,nullptr);
    frame_->blas.clear(); frame_->blas_addresses.clear(); frame_->blas_used=0;
    for (auto& buffer : frame_->retired) destroy(buffer);
    frame_->retired.clear();
    frame_->stats={};
}
void RtScene::record_packet(VkCommandBuffer command, std::span<const RtGeometry> geometries) {
    if (!frame_ || geometries.empty()) return;
    Frame& frame=*frame_;
    std::vector<RtGeometry> items(geometries.begin(),geometries.end());
    VkDeviceSize positions_bytes=0;
    for (auto& item : items) { item.positions_out=positions_bytes; positions_bytes+=align(VkDeviceSize(item.vertex_count)*12,16); }
    ensure(frame,frame.positions,positions_bytes,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,false);
    std::vector<VkAccelerationStructureGeometryKHR> triangles(items.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(items.size());
    std::vector<uint32_t> counts(items.size());
    for (size_t i=0;i<items.size();++i) {
        auto& item=items[i];
        item.positions_out+=frame.positions.address;
        auto& geometry=triangles[i];
        geometry.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR; geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags=item.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        auto& data=geometry.geometry.triangles;
        data.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        data.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT; data.vertexData.deviceAddress=item.positions_out; data.vertexStride=12; data.maxVertex=item.vertex_count ? item.vertex_count-1 : 0;
        data.indexType=VK_INDEX_TYPE_UINT32; data.indexData.deviceAddress=item.indices+VkDeviceAddress(item.first_index)*4;
        counts[i]=item.index_count/3;
        ranges[i]={counts[i],0,0,0};
        frame.stats.triangles+=counts[i];
    }
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; build.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    build.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; build.geometryCount=uint32_t(triangles.size()); build.pGeometries=triangles.data();
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    build_sizes_(device_,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&build,counts.data(),&sizes);
    if (ensure(frame,frame.blas_storage,align(frame.blas_used,256)+sizes.accelerationStructureSize,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,false)) frame.blas_used=0;
    ensure(frame,frame.scratch,sizes.buildScratchSize+scratch_alignment_,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,false);
    const VkDeviceSize offset=align(frame.blas_used,256);   // acceleration structure offsets are multiples of 256
    VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    create.buffer=frame.blas_storage.buffer; create.offset=offset; create.size=sizes.accelerationStructureSize; create.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkAccelerationStructureKHR blas; check(create_as_(device_,&create,nullptr,&blas),"BLAS");
    frame.blas.push_back(blas); frame.blas_used=offset+sizes.accelerationStructureSize;
    VkAccelerationStructureDeviceAddressInfoKHR address{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR}; address.accelerationStructure=blas;
    frame.blas_addresses.push_back(as_address_(device_,&address));
    build.dstAccelerationStructure=blas; build.scratchData.deviceAddress=align(frame.scratch.address,scratch_alignment_);
    // The previous build (this frame's earlier packet) is done with the positions and the scratch before they are written again.
    memory_barrier(command,VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT|VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,transform_);
    for (const auto& item : items) {
        const TransformPush push{item.vertices,item.positions_out,item.palettes,item.vertex_count,item.first_vertex,item.palette,item.current,item.indexed};
        vkCmdPushConstants(command,transform_layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,&push);
        vkCmdDispatch(command,(item.vertex_count+63)/64,1,1);
    }
    memory_barrier(command,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_2_SHADER_READ_BIT);
    const VkAccelerationStructureBuildRangeInfoKHR* range_pointer=ranges.data();
    build_(command,1,&build,&range_pointer);
    ++frame.stats.blas; frame.stats.geometries+=uint32_t(items.size());
}
bool RtScene::build_tlas(VkCommandBuffer command) {
    if (!frame_ || frame_->blas.empty()) return false;
    Frame& frame=*frame_;
    const uint32_t count=uint32_t(frame.blas.size());
    ensure(frame,frame.instances,VkDeviceSize(count)*sizeof(VkAccelerationStructureInstanceKHR),VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,true);
    auto* instances=static_cast<VkAccelerationStructureInstanceKHR*>(frame.instances.mapped);
    for (uint32_t i=0;i<count;++i) {
        VkAccelerationStructureInstanceKHR instance{};
        instance.transform.matrix[0][0]=instance.transform.matrix[1][1]=instance.transform.matrix[2][2]=1;   // the BLASes are in view space already
        instance.instanceCustomIndex=i; instance.mask=0xFF;
        instance.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference=frame.blas_addresses[i];
        instances[i]=instance;
    }
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR}; geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR; geometry.geometry.instances.data.deviceAddress=frame.instances.address;
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; build.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    build.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; build.geometryCount=1; build.pGeometries=&geometry;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    build_sizes_(device_,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&build,&count,&sizes);
    // The TLAS spans its whole storage buffer, so its handle is recreated only when the buffer grows (the slot's previous frame has completed).
    if (ensure(frame,frame.tlas_storage,sizes.accelerationStructureSize,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,false) || !frame.tlas) {
        if (frame.tlas) destroy_as_(device_,frame.tlas,nullptr);
        VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        create.buffer=frame.tlas_storage.buffer; create.size=frame.tlas_storage.size; create.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        check(create_as_(device_,&create,nullptr,&frame.tlas),"TLAS");
    }
    ensure(frame,frame.scratch,sizes.buildScratchSize+scratch_alignment_,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,false);
    build.dstAccelerationStructure=frame.tlas; build.scratchData.deviceAddress=align(frame.scratch.address,scratch_alignment_);
    memory_barrier(command,VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    const VkAccelerationStructureBuildRangeInfoKHR range{count,0,0,0}; const auto* range_pointer=&range;
    build_(command,1,&build,&range_pointer);
    memory_barrier(command,VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    return true;
}
void RtScene::record_probe(VkCommandBuffer command, const RtProbeInputs& in) {
    if (!frame_ || !frame_->tlas || !in.region.width || !in.region.height) return;
    Frame& frame=*frame_;
    {   // the slot's set: its previous frame has completed, so it can be rewritten
        VkWriteDescriptorSetAccelerationStructureKHR tlas{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR}; tlas.accelerationStructureCount=1; tlas.pAccelerationStructures=&frame.tlas;
        const VkDescriptorImageInfo depth{VK_NULL_HANDLE,in.depth,VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorBufferInfo counter{counters_.buffer,kCounterStride*slot_,sizeof(uint32_t)};
        VkWriteDescriptorSet writes[3]{};
        for (uint32_t i=0;i<3;++i) { writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=frame.probe_set; writes[i].dstBinding=i; writes[i].descriptorCount=1; }
        writes[0].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; writes[0].pNext=&tlas;
        writes[1].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[1].pImageInfo=&depth;
        writes[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[2].pBufferInfo=&counter;
        vkUpdateDescriptorSets(device_,3,writes,0,nullptr);
    }
    vkCmdFillBuffer(command,counters_.buffer,kCounterStride*slot_,sizeof(uint32_t),0);
    memory_barrier(command,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    ProbeParams params{};
    std::copy(in.viewport.begin(),in.viewport.begin()+4,params.viewport);
    params.view_a[0]=in.view.x_scale; params.view_a[1]=in.view.y_scale; params.view_a[2]=in.view.x_offset; params.view_a[3]=in.view.y_offset;
    params.view_b[0]=in.view.depth_a; params.view_b[1]=in.view.depth_b;
    params.depth_range[0]=in.viewport[4]; params.depth_range[1]=in.viewport[5];
    std::copy(in.to_light.begin(),in.to_light.end(),params.to_light);
    params.region[0]=in.region.width; params.region[1]=in.region.height;
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,probe_);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,probe_layout_,0,1,&frame.probe_set,0,nullptr);
    vkCmdPushConstants(command,probe_layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,&params);
    vkCmdDispatch(command,(in.region.width+7)/8,(in.region.height+7)/8,1);
    memory_barrier(command,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
    probe_slot_=slot_;
}
uint32_t RtScene::probe_occluded() const {
    if (probe_slot_==UINT32_MAX) return 0;
    uint32_t value; std::memcpy(&value,static_cast<const char*>(counters_.mapped)+kCounterStride*probe_slot_,sizeof value);
    return value;
}
VkAccelerationStructureKHR RtScene::tlas() const { return frame_ ? frame_->tlas : VK_NULL_HANDLE; }
void RtScene::shutdown() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    for (auto& frame : frames_) {
        for (auto blas : frame.blas) destroy_as_(device_,blas,nullptr);
        if (frame.tlas) destroy_as_(device_,frame.tlas,nullptr);
        for (auto& buffer : frame.retired) destroy(buffer);
        for (Buffer* buffer : {&frame.positions,&frame.scratch,&frame.blas_storage,&frame.tlas_storage,&frame.instances}) destroy(*buffer);
    }
    destroy(counters_);
    if (pool_) vkDestroyDescriptorPool(device_,pool_,nullptr);
    for (auto pipeline : {transform_,probe_}) if (pipeline) vkDestroyPipeline(device_,pipeline,nullptr);
    for (auto layout : {transform_layout_,probe_layout_}) if (layout) vkDestroyPipelineLayout(device_,layout,nullptr);
    if (probe_set_layout_) vkDestroyDescriptorSetLayout(device_,probe_set_layout_,nullptr);
    for (auto module : {transform_module_,probe_module_}) if (module) vkDestroyShaderModule(device_,module,nullptr);
    *this=RtScene{};
}
}
