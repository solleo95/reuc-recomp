#include "check.h"
#include "test_draws.h"
#include <cmath>
static gx::DrawPacket draw(uint32_t callsite,std::initializer_list<std::array<float,3>> points) {
    gx::DrawPacket d; d.callsite=callsite;
    for(auto p:points) { gx::Vertex v; v.position=p; v.position_matrix=0; d.vertices.push_back(v); }
    return d;
}
int main() {
    // Identity projection and position matrix: view = model, aim y points down.
    gx::FramePacket packet;
    packet.draws.push_back(draw(0x8001,{{0.5f,-0.25f,0},{-0.5f,0.75f,0}}));
    packet.draws.push_back(draw(0x8001,{{0.9f,0.0f,0}}));
    packet.draws.push_back(draw(0x8002,{{0,0,0}}));
    { auto texture=std::make_shared<gx::TexturePixels>(); texture->levels.resize(1); texture->levels[0].width=64; texture->levels[0].height=32; packet.draws[2].texture=texture; }
    const auto stats=uc::summarize_draws(packet);
    CHECK(stats.size()==3);
    CHECK(stats[0].callsite==0x8001 && stats[0].vertices==2 && stats[1].vertices==1 && stats[2].callsite==0x8002);
    CHECK(std::abs(stats[0].x0+0.5f)<1e-6f && std::abs(stats[0].x1-0.5f)<1e-6f && std::abs(stats[0].y0+0.75f)<1e-6f && std::abs(stats[0].y1-0.25f)<1e-6f);
    // Vertices behind the camera (w <= 0) are skipped; a draw with none is dropped.
    gx::FramePacket behind; auto d=draw(0x8003,{{0,0,0}}); d.transforms.edit().projection[3]={0,0,0,-1}; behind.draws.push_back(d);
    CHECK(uc::summarize_draws(behind).empty());
    const auto json=uc::test_draws_json(stats);
    CHECK(json.find(R"({"pass":0,"callsite":32769,"vertices":2,"tex":[0,0,0],"bbox":[-0.5000,-0.7500,0.5000,0.2500]})")!=std::string::npos);
    CHECK(stats[2].tex_w==64 && stats[2].tex_h==32 && stats[2].tex_id!=0 && stats[0].tex_id==0);
    CHECK(json.find(R"("tex":[64,32,)")!=std::string::npos);
    // Scene projection: the first perspective projection (row 3 = 0,0,-1,0) of the packet, row-major; none -> all zeros.
    const std::array<float,16> zero{};
    CHECK(uc::scene_projection(packet)==zero);
    gx::FramePacket scene; auto p=draw(0x8004,{{0,0,-1}}); p.transforms.edit().projection={{{1.5f,0,0,0},{0,2.5f,0,0},{0,0,-1.1f,-0.2f},{0,0,-1,0}}}; scene.draws.push_back(draw(0x8005,{{0,0,0}})); scene.draws.push_back(p);
    const auto proj=uc::scene_projection(scene); CHECK(proj[0]==1.5f && proj[5]==2.5f && proj[10]==-1.1f && proj[11]==-0.2f && proj[14]==-1);
    CHECK(uc::projection_json(zero)=="" && uc::projection_json(proj).substr(0,8)=="[1.5,0,0");
    uc::test_draws_publish(stats); CHECK(uc::test_draws_latest().size()==3);
    return 0;
}
