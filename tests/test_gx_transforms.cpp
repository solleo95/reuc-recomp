#include "check.h"
#include "gx_vertices.h"
#include "gx_transforms.h"
#include <cmath>
#include <limits>

static std::array<float,4> clip(const gx::GpuVertex& vertex,const gx::Projection& projection) {
    std::array<float,4> view{0,0,0,1},out{};
    for (size_t r=0;r<3;++r) {
        view[r]=vertex.position_rows[r][3];
        for (size_t c=0;c<3;++c) view[r]+=vertex.position_rows[r][c]*vertex.position[c];
    }
    for (size_t r=0;r<4;++r) for (size_t c=0;c<4;++c) out[r]+=projection[r][c]*view[c];
    return out;
}
template<class Fn> static void rejects(Fn fn) {
    bool caught=false;
    try { fn(); } catch (const gx::Error&) { caught=true; }
    CHECK(caught);
}
int main() {
    static_assert(sizeof(gx::GpuVertex)==156);
    gx::Transforms transforms;
    gx::Vertex vertex; vertex.position={1,2,3}; vertex.color[0]={1,2,3,4};
    vertex.texcoord[0]={0.25f,0.75f};
    vertex.texcoord[7]={0.125f,0.875f};
    auto uploaded=gx::gpu_vertex(vertex,transforms,false,0x80000000);
    CHECK((clip(uploaded,transforms.projection)==std::array<float,4>{1,2,3,1}));
    CHECK(uploaded.color==vertex.color[0]);
    CHECK((uploaded.texcoords[0]==std::array<float,2>{0.25f,0.75f}));
    CHECK((uploaded.texcoords[7]==std::array<float,2>{0.125f,0.875f}));
    vertex.normal[0]=1; vertex.normal[1]=2; vertex.normal[2]=3;
    gx::set_normal_matrix(transforms,0,{{{0,1,0},{-1,0,0},{0,0,2}}});
    uploaded=gx::gpu_vertex(vertex,transforms,false,0);
    CHECK((uploaded.normal==std::array<float,3>{2,-1,6}));
    CHECK((uploaded.object_normal==std::array<float,3>{1,2,3})); // texgen reads the untransformed normal
    CHECK(uploaded.color1==vertex.color[1]);
    rejects([&]{gx::set_normal_matrix(transforms,62,{});});
    const gx::PositionMatrix translated{{{1,0,0,4},{0,2,0,5},{0,0,3,6}}};
    gx::set_position_matrix(transforms,3,translated);
    transforms.current=3;
    uploaded=gx::gpu_vertex(vertex,transforms,false,0x80000000);
    CHECK((clip(uploaded,transforms.projection)==std::array<float,4>{5,9,15,1}));
    vertex.position_matrix=0;
    CHECK((clip(gx::gpu_vertex(vertex,transforms,true,0),transforms.projection)==std::array<float,4>{1,2,3,1}));
    vertex.position_matrix=6;
    rejects([&]{gx::gpu_vertex(vertex,transforms,true,0);});
    rejects([&]{gx::set_position_matrix(transforms,62,translated);});
    gx::set_position_matrix(transforms,61,translated);
    vertex.position_matrix=61;
    CHECK(gx::gpu_vertex(vertex,transforms,true,0).position_rows==translated);
    // Position rows32..63 select the lower normal palette via the low five bits.
    gx::set_position_matrix(transforms,33,translated);
    gx::set_normal_matrix(transforms,1,{{{0,1,0},{-1,0,0},{0,0,2}}});
    gx::set_normal_matrix(transforms,33,{{{1,0,0},{0,1,0},{0,0,1}}});
    vertex.position_matrix=33;
    uploaded=gx::gpu_vertex(vertex,transforms,true,0);
    CHECK(uploaded.position_rows==translated);
    CHECK((uploaded.normal==std::array<float,3>{2,-1,6}));
    transforms.current=33;
    CHECK((gx::gpu_vertex(vertex,transforms,false,0).normal==std::array<float,3>{2,-1,6}));
    // Non-finite entries are guest data (an animation that went wrong): the hardware loads them and the vertices they transform come out non-finite,
    // which the rasterizer drops. Only a row outside the 64-row register file is an error.
    auto invalid=translated; invalid[0][3]=std::numeric_limits<float>::quiet_NaN(); invalid[1][0]=std::numeric_limits<float>::infinity();
    gx::set_position_matrix(transforms,12,invalid);
    CHECK(std::isnan(transforms.position_rows[12][3]) && std::isinf(transforms.position_rows[13][0]) && transforms.loaded_rows[12]);
    gx::set_normal_matrix(transforms,5,{{{std::numeric_limits<float>::quiet_NaN(),0,0},{0,1,0},{0,0,1}}});
    CHECK(std::isnan(transforms.normal_rows[5][0]) && transforms.loaded_normal_rows[5]);
    vertex.position_matrix=12; vertex.position={1,1,1};
    const auto dropped=gx::gpu_vertex(vertex,transforms,true,0);   // no exception; the position carries the NaN
    CHECK(std::isnan(dropped.position_rows[0][3]));
    rejects([&]{gx::set_position_matrix(transforms,62,translated);});
    CHECK(transforms.position_rows[0][0]==1);

    const auto ortho=gx::make_projection({1,0,1,0,1,0},1);
    vertex.position={0.25f,0.5f,-1}; vertex.position_matrix=0;
    auto projected=clip(gx::gpu_vertex(vertex,transforms,true,0),ortho);
    CHECK((projected==std::array<float,4>{0.25f,-0.5f,0,1}));
    vertex.position[2]=0;
    CHECK(clip(gx::gpu_vertex(vertex,transforms,true,0),ortho)[2]==1);
    const auto perspective=gx::make_projection({1,0,1,0,-1.f/9,-10.f/9},0);
    vertex.position={0.25f,0.5f,-1};
    projected=clip(gx::gpu_vertex(vertex,transforms,true,0),perspective);
    CHECK(std::abs(projected[2])<1e-6f && projected[3]==1);
    vertex.position[2]=-10;
    projected=clip(gx::gpu_vertex(vertex,transforms,true,0),perspective);
    CHECK(std::abs(projected[2]/projected[3]-1)<1e-6f && projected[3]==10);
    rejects([&]{gx::make_projection({1,0,1,0,1,0},2);});
    rejects([&]{gx::make_projection({1,0,1,0,std::numeric_limits<float>::quiet_NaN(),0},0);});
    // The GPU path (static GpuMeshVertex + PaletteBlock, evaluated like smoke.slang does) must equal gpu_vertex().
    {
        gx::Transforms t;
        t.current=6;
        gx::set_position_matrix(t,6,{{{1,0,0,5},{0,2,0,6},{0,0,3,7}}});
        gx::set_position_matrix(t,9,{{{0,1,0,1},{-1,0,0,2},{0,0,1,3}}});
        gx::set_normal_matrix(t,6,{{{2,0,0},{0,3,0},{0,0,4}}});   // normal rows 6..8 loaded; rows 9..11 (position matrix at 9) are not
        gx::PaletteBlock block; gx::fill_palette(block,t);
        auto shader=[&](const gx::Vertex& v,bool indexed) {
            const gx::GpuMeshVertex m=gx::mesh_vertex(v);
            const uint32_t row=indexed ? m.row : t.current, nr=row&31;
            bool all=true;
            for (uint32_t i=0;i<3;++i) { const uint32_t r=nr+i; const uint32_t word=r<32 ? block.loaded[0] : block.loaded[1]; all=all && ((word>>(r&31))&1u); }
            std::array<float,3> normal=m.object_normal;
            if (all) for (size_t i=0;i<3;++i) { const auto& n=block.normal[nr+i]; normal[i]=n[0]*m.object_normal[0]+n[1]*m.object_normal[1]+n[2]*m.object_normal[2]; }
            return std::pair{std::array{block.position[row],block.position[row+1],block.position[row+2]},normal};
        };
        gx::Vertex v; v.position={1,2,3}; v.normal={1,-1,2,0,0,0,0,0,0};
        for (bool indexed : {false,true}) for (uint8_t row : {uint8_t(6),uint8_t(9)}) {
            v.position_matrix=row;
            const auto cpu=gx::gpu_vertex(v,t,indexed,0); const auto gpu=shader(v,indexed);
            CHECK(gpu.first[0]==cpu.position_rows[0] && gpu.first[1]==cpu.position_rows[1] && gpu.first[2]==cpu.position_rows[2]);
            CHECK(gpu.second==cpu.normal);
        }
        const gx::Vertex rows[]={[]{ gx::Vertex x; x.position_matrix=9; return x; }(),[]{ gx::Vertex x; x.position_matrix=6; return x; }()};
        CHECK(gx::used_rows(rows)==((uint64_t(1)<<9)|(uint64_t(1)<<6)));
        gx::validate_draw_rows(t,true,gx::used_rows(rows),0);
        gx::validate_draw_rows(t,false,0,0);
        gx::Vertex unloaded; unloaded.position_matrix=20;
        rejects([&]{gx::validate_draw_rows(t,true,gx::used_rows(std::span(&unloaded,1)),0);});   // rows 20..22 were never loaded
        gx::Vertex far_row; far_row.position_matrix=62;
        rejects([&]{gx::validate_draw_rows(t,true,gx::used_rows(std::span(&far_row,1)),0);});     // above 61
        gx::Vertex wild; wild.position_matrix=200;
        rejects([&]{gx::validate_draw_rows(t,true,gx::used_rows(std::span(&wild,1)),0);});        // past the mask: still rejected
        t.current=30; rejects([&]{gx::validate_draw_rows(t,false,0,0);});
    }
}
