#pragma once
#include <array>
#include <bitset>
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
    std::array<float,3> texcoord;
    std::array<std::array<float,3>,7> extra_texcoords;
    std::array<float,3> normal;
    std::array<uint8_t,4> color1;
};
struct Vertex;
Projection make_projection(const std::array<float,6>& coefficients,uint32_t type);
void set_position_matrix(Transforms& transforms,uint32_t row,const PositionMatrix& matrix);
void set_normal_matrix(Transforms& transforms,uint32_t row,const NormalMatrix& matrix);
GpuVertex gpu_vertex(const Vertex& vertex,const Transforms& transforms,bool indexed,uint32_t callsite);
}
