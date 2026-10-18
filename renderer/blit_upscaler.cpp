#include "upscaler.h"

namespace uc {
namespace {
class BlitUpscaler final : public Upscaler {
public:
    void evaluate(const UpscaleInputs& in) override {
        VkImageBlit blit{}; blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        blit.srcOffsets[1]={int32_t(in.render_size.width),int32_t(in.render_size.height),1};
        blit.dstOffsets[1]={int32_t(in.output_size.width),int32_t(in.output_size.height),1};
        vkCmdBlitImage(in.command,in.color.image,VK_IMAGE_LAYOUT_GENERAL,in.output.image,VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_LINEAR);
    }
    bool sharpens() const override { return false; }
    RenderExtent minimum_internal(RenderExtent) const override { return {1,1}; }
};
}
std::unique_ptr<Upscaler> make_blit_upscaler() { return std::make_unique<BlitUpscaler>(); }
}
