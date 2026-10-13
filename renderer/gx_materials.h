#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <cstddef>
namespace gx {
struct TevStage { uint32_t color=0,alpha=0,order=0,konst=0,indirect=0; };
struct TevMaterial {
    uint32_t stages=0,indirect_order=0;
    std::array<uint8_t,4> swaps{};
    std::array<TevStage,16> stage{};
};
struct LightUniform {
    std::array<float,4> color{},cosine{1,0,0,0},distance{1,0,0,0},position{},direction{0,0,-1,0};
};
struct alignas(16) DrawUniform {
    std::array<std::array<float,4>,4> registers{};
    std::array<std::array<float,4>,4> konst{{{1,1,1,1},{1,1,1,1},{1,1,1,1},{1,1,1,1}}};
    std::array<std::array<float,4>,16> tex_rows{};
    std::array<uint32_t,8> tex_sources{0,1,2,3,4,5,6,7};
    std::array<uint32_t,4> pixel{0x3F0000,0,0,2}; // alpha compare, depth, destination alpha, color channel count
    std::array<LightUniform,8> lights;
    std::array<std::array<float,4>,2> materials{{{1,1,1,1},{1,1,1,1}}},ambient{};
    std::array<uint32_t,4> channels{1,1,1,1}; // color0,color1,alpha0,alpha1
    DrawUniform() { for(size_t i=0;i<8;++i) { tex_rows[i*2]={1,0,0,0}; tex_rows[i*2+1]={0,1,0,0}; } }
};
static_assert(sizeof(DrawUniform)==1152);
struct PixelMaterial { uint32_t alpha,depth,blend,destination_alpha; };
PixelMaterial import_pixel_material(std::span<const std::byte> bytes);
struct ColorMaterial {
    std::array<std::array<float,4>,3> registers;
    std::array<std::array<float,4>,4> konst;
};
ColorMaterial import_color_material(std::span<const std::byte> bytes);
struct MeshSetup {
    bool descriptors=false;
    uint32_t low=0,high=0;
    std::array<uint32_t,3> vat{};
    std::array<uint32_t,12> addresses{},strides{};
    uint32_t arrays=0;
};
MeshSetup import_mesh_setup(std::span<const std::byte> bytes);
size_t tev_material_size(uint32_t stages);
TevMaterial import_tev_material(uint32_t stages,std::span<const std::byte> bytes);
}
