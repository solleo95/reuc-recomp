#pragma once
#include <cstdint>
#include <array>
#include <filesystem>
#include <vector>

namespace uc {
struct RenderPixels { uint64_t serial = 0; uint32_t width = 0, height = 0; std::vector<uint8_t> rgba; };
struct MotionPixels { uint32_t width=0,height=0; std::vector<float> rg; std::array<float,2> at(uint32_t x,uint32_t y) const {const size_t i=(size_t(y)*width+x)*2;return {rg.at(i),rg.at(i+1)};} };
void save_png(const RenderPixels& pixels,const std::filesystem::path& path);
}
