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
    std::array<std::array<float,4>,24> tex_rows{}; // texture matrix (3 rows) per texgen; identity for GX_IDENTITY
    std::array<uint32_t,8> tex_gens{}; // raw XF 0x1040+n texgen control (projection, input form, type, source row)
    std::array<uint32_t,8> tex_posts{}; // raw XF 0x1050+n post-transform word (bit 8 normalize)
    std::array<std::array<float,4>,24> post_rows{}; // dual-texture post matrix (3 rows) per texgen
    std::array<uint32_t,4> pixel{0x3F0000,0,0,2}; // alpha compare, depth, destination alpha, color channel count
    std::array<LightUniform,8> lights;
    std::array<std::array<float,4>,2> materials{{{1,1,1,1},{1,1,1,1}}},ambient{};
    std::array<uint32_t,4> channels{1,1,1,1}; // color0,color1,alpha0,alpha1
    std::array<uint32_t,8> fog{}; // raw BP 0xEE..0xF2 (A, B magnitude, B shift, C/proj/fsel, color); fsel 0 = off
    std::array<uint32_t,12> indirect{}; // raw BP 0x06..0x0E (three indirect matrices), 0x27 IREF, 0x25/0x26 RAS1_SS0/1
    std::array<uint32_t,16> ind_cmd{}; // per TEV stage BP 0x10+n IND_CMD; 0 = direct
    DrawUniform() {
        for(size_t i=0;i<8;++i) {
            tex_gens[i]=uint32_t(5+i)<<7; // regular ST from UVi, AB11
            for(size_t row=0;row<3;++row) { tex_rows[i*3+row]={}; tex_rows[i*3+row][row]=1; post_rows[i*3+row]={}; post_rows[i*3+row][row]=1; }
        }
    }
};
static_assert(sizeof(DrawUniform)==1840);
// Texture matrices by GX matrix id (0..125; 60 identity): 3 rows each, what a per-vertex texture matrix index (VCD TexNMtxIdx) selects from.
// Attached to a draw only when its vertex format carries those indices.
struct TextureBank {
    std::array<std::array<float,4>,126*3> rows;
    TextureBank() { for(size_t i=0;i<rows.size();++i) rows[i]={float(i%3==0),float(i%3==1),float(i%3==2),0}; }
    bool operator==(const TextureBank&) const = default;
};
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
