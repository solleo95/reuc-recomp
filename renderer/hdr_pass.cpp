#include "hdr_pass.h"
#include <fstream>
#include <stdexcept>
#include <string>

namespace uc {
namespace {
void check(VkResult result, const char* what) { if (result != VK_SUCCESS) throw std::runtime_error(std::string("HDR: ") + what + " failed: VkResult " + std::to_string(result)); }
struct Constants { int32_t origin[2], size[2], extent[2]; float paper, peak, ui; uint32_t has_overlay; };
constexpr uint32_t kMaxSets=64;   // slots x {with, without overlay} + the EFB; far below this
}
void HdrPass::initialize(VkDevice device, const std::filesystem::path& shaders) {
    if (pipeline_) return;
    device_ = device;
    const auto load=[&](const char* name, VkShaderModule& module) {
        std::ifstream file(shaders / name, std::ios::binary);
        std::vector<char> code((std::istreambuf_iterator<char>(file)), {});
        if (code.empty() || code.size() % 4) throw std::runtime_error(std::string("HDR: shader ") + name + " missing or invalid");
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = code.size(); info.pCode = reinterpret_cast<const uint32_t*>(code.data());
        check(vkCreateShaderModule(device_, &info, nullptr, &module), "shader module");
    };
    load("hdr.vert.spv", vertex_); load("hdr.frag.spv", fragment_);
    const VkDescriptorSetLayoutBinding bindings[] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount = 2; set.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(device_, &set, nullptr, &set_layout_), "descriptor layout");
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Constants)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount = 1; layout.pSetLayouts = &set_layout_; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_), "pipeline layout");
    const VkPipelineShaderStageCreateInfo stages[] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex_, "vertexMain", nullptr},
                                                      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment_, "fragmentMain", nullptr}};
    VkPipelineVertexInputStateCreateInfo vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &attachment;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &kFormat;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pipeline.pNext = &rendering;
    pipeline.stageCount = 2; pipeline.pStages = stages; pipeline.pVertexInputState = &vertices; pipeline.pInputAssemblyState = &assembly; pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster; pipeline.pMultisampleState = &multisample; pipeline.pColorBlendState = &blend; pipeline.pDynamicState = &dynamic; pipeline.layout = layout_;
    check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_), "pipeline");
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * kMaxSets};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = kMaxSets; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
    check(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_), "descriptor pool");
}
void HdrPass::record(VkCommandBuffer command, VkImageView source, VkOffset2D origin, VkExtent2D size, VkImageView overlay,
                     VkImageView target, VkExtent2D extent, const HdrSettings& settings) {
    if (!pipeline_) throw std::logic_error("HDR pass not initialized");
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (const auto& bound : bound_) if (bound.source == source && bound.overlay == overlay) set = bound.set;
    if (!set) {
        if (bound_.size() >= kMaxSets) throw std::logic_error("HDR pass: more source images than descriptor sets");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool = pool_; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &set_layout_;
        check(vkAllocateDescriptorSets(device_, &allocate, &set), "descriptor set");
        const VkDescriptorImageInfo images[] = {{VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, overlay ? overlay : source, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = set; write.dstBinding = 0; write.descriptorCount = 2;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; write.pImageInfo = images;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        bound_.push_back({source, overlay, set});
    }
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; color.imageView = target; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea.extent = extent; rendering.layerCount = 1; rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &color;
    vkCmdBeginRendering(command, &rendering);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1}; const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &viewport); vkCmdSetScissor(command, 0, 1, &scissor);
    const Constants constants{{origin.x, origin.y}, {int32_t(size.width), int32_t(size.height)}, {int32_t(extent.width), int32_t(extent.height)},
                              float(settings.paper_white_nits), float(settings.peak_nits), float(settings.ui_nits), overlay ? 1u : 0u};
    vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, &constants);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRendering(command);
}
void HdrPass::forget() {
    if (!pool_ || bound_.empty()) return;
    check(vkResetDescriptorPool(device_, pool_, 0), "descriptor pool reset");
    bound_.clear();
}
void HdrPass::shutdown() {
    if (!device_) return;
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (vertex_) vkDestroyShaderModule(device_, vertex_, nullptr);
    if (fragment_) vkDestroyShaderModule(device_, fragment_, nullptr);
    *this = HdrPass{};
}
}
