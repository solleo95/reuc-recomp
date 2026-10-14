#pragma once
#include "vulkan_renderer.h"

namespace fifo_replay {
struct ImageComparison {
    uc::RenderPixels diff;
    double mean=0;
    uint32_t peak=0;
    uint64_t changed_pixels=0;
};
uc::RenderPixels load_reference(const std::filesystem::path& path);
// Absolute RGB error in encoded 8-bit values; presentation alpha is ignored.
ImageComparison compare_images(const uc::RenderPixels& actual,const uc::RenderPixels& reference);
}
