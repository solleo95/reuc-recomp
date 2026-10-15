#pragma once
#include <array>
#include <bitset>
#include <span>
#include <cstdint>

namespace gx {
using Projection=std::array<std::array<float,4>,4>;
using PositionMatrix=std::array<std::array<float,4>,3>;
using NormalMatrix=std::array<std::array<float,3>,3>;
struct Transforms {
    Projection projection{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    std::array<std::array<float,4>,64> position_rows{{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
    std::bitset<64> loaded_rows{7};
    uint32_t current=0;
    std::array<std::array<float,3>,64> normal_rows{{{1,0,0},{0,1,0},{0,0,1}}};
    std::bitset<64> loaded_normal_rows{7};
};
struct GpuVertex {
    std::array<float,3> position;
    std::array<uint8_t,4> color;
    PositionMatrix position_rows;
    std::array<std::array<float,2>,8> texcoords;
    std::array<float,3> normal; // view space, for lighting
    std::array<uint8_t,4> color1;
    std::array<float,3> object_normal; // untransformed, the texgen normal source
};
struct Vertex;
// What the GPU keeps per mesh vertex: the attributes only. Matrices come from a per-draw palette (PaletteBlock), so a mesh is uploaded once and
// drawn with any transform. `row` is the vertex's position matrix row; it selects the palette entry when the draw uses per-vertex matrices.
struct GpuMeshVertex {
    std::array<float,3> position;
    std::array<uint8_t,4> color;
    std::array<std::array<float,2>,8> texcoords;
    std::array<float,3> object_normal;
    std::array<uint8_t,4> color1;
    uint32_t row;
};
static_assert(sizeof(GpuMeshVertex)==100);
GpuMeshVertex mesh_vertex(const Vertex& vertex);
// A Transforms block as the vertex shader reads it from a storage buffer (std430, 2064 bytes). `loaded` = bit mask of the loaded normal rows.
struct alignas(16) PaletteBlock {
    std::array<std::array<float,4>,64> position;
    std::array<std::array<float,4>,64> normal;   // xyz used
    std::array<uint32_t,4> loaded;               // x: normal rows 0..31, y: 32..63
};
static_assert(sizeof(PaletteBlock)==2064);
void fill_palette(PaletteBlock& block, const Transforms& transforms);
// Bit mask of the position matrix rows used by `vertices`' per-vertex matrix index.
uint64_t used_rows(std::span<const Vertex> vertices);
// Throws the errors gpu_vertex() throws for a row that is out of range or not loaded: the current row, or every row in `rows` when indexed.
void validate_draw_rows(const Transforms& transforms, bool indexed, uint64_t rows, uint32_t callsite);
Projection make_projection(const std::array<float,6>& coefficients,uint32_t type);
void set_position_matrix(Transforms& transforms,uint32_t row,const PositionMatrix& matrix);
void set_normal_matrix(Transforms& transforms,uint32_t row,const NormalMatrix& matrix);
GpuVertex gpu_vertex(const Vertex& vertex,const Transforms& transforms,bool indexed,uint32_t callsite);
}
