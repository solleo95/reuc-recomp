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
    auto invalid_normal=gx::NormalMatrix{}; invalid_normal[0][0]=std::numeric_limits<float>::quiet_NaN();
    rejects([&]{gx::set_normal_matrix(transforms,0,invalid_normal);});
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
    auto invalid=translated; invalid[0][0]=std::numeric_limits<float>::infinity();
    rejects([&]{gx::set_position_matrix(transforms,0,invalid);});
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
}
