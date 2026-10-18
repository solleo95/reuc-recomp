#include "check.h"
#include "scene_boundary.h"
#include <fstream>
#include <map>
#include <sstream>
#include <string>
// Frames recorded with UC_SCENE_DUMP: "F <index> <serial>" then the packet summaries of that frame.
static std::map<std::string,std::vector<gx::PacketSummary>> recorded_frames(const std::string& path) {
    std::ifstream in(path); CHECK(in.good());
    std::map<std::string,std::vector<gx::PacketSummary>> frames;
    std::string line, index, chunk;
    const auto flush=[&] { if(!index.empty()) { std::istringstream s(chunk); frames[index]=gx::read_packets(s); } chunk.clear(); };
    while(std::getline(in,line)) {
        if(line.rfind("F ",0)==0) { flush(); std::istringstream s(line.substr(2)); s>>index; }
        else chunk+=line+'\n';
    }
    flush();
    return frames;
}
int main() {
    {   // cut positions on recorded frames (train chapter: title, menus, gameplay, boss fight with a glow pre-pass, results)
        const auto frames=recorded_frames(UC_SCENE_DATA "/train_chapter.txt");
        std::ifstream expected(UC_SCENE_DATA "/train_chapter.expected"); CHECK(expected.good());
        std::string line; size_t checked=0;
        gx::SceneBoundary boundary;
        while(std::getline(expected,line)) {
            if(line.empty() || line[0]=='#') continue;
            std::istringstream s(line); std::string index,packet,draw; s>>index>>packet>>draw;
            const auto& packets=frames.at(index);
            std::optional<std::pair<size_t,size_t>> cut;
            boundary.reset();
            for(size_t p=0;p<packets.size();++p) if(auto at=boundary.on_packet(packets[p])) { CHECK(!cut); cut=std::pair{p,*at}; }
            if(packet=="-") CHECK(!cut && !boundary.frame_had_scene());
            else {
                CHECK(cut && cut->first==std::stoul(packet) && cut->second==std::stoul(draw) && boundary.frame_had_scene());
                CHECK(boundary.scene_viewport() && (*boundary.scene_viewport())[2]==640 && (*boundary.scene_viewport())[3]==360);
            }
            ++checked;
        }
        CHECK(checked==7);
    }
    {   // the state of a frame ends at its present: the next frame starts uncut
        gx::SceneBoundary boundary;
        gx::PacketSummary scene; scene.draws.push_back({0x80366E14,true,true,true,std::array<float,6>{0,0,640,360,0,1},3,0,1,0});
        gx::PacketSummary copy; copy.copy_source=std::array<uint32_t,4>{0,0,640,360};
        gx::PacketSummary present; present.present=true;
        CHECK(!boundary.on_packet(scene) && boundary.on_packet(copy)==0u && !boundary.on_packet(present));
        CHECK(boundary.frame_had_scene());
        CHECK(!boundary.on_packet(scene) && boundary.on_packet(copy)==0u);   // next frame cuts again
        gx::PacketSummary small; small.copy_source=std::array<uint32_t,4>{0,0,320,180};
        boundary.on_packet(present);
        CHECK(!boundary.on_packet(scene) && !boundary.on_packet(small));   // a quarter copy is not the scene copy
        boundary.on_packet(present);
        CHECK(!boundary.on_packet(copy) && !boundary.on_packet(present) && !boundary.frame_had_scene());   // no perspective draw: no scene
    }
    {   // summary of a draw
        gx::DrawPacket draw; draw.callsite=0x80366E14;
        gx::Transforms t; t.projection={{{1,0,0,0},{0,1,0,0},{0,0,-1,-1},{0,0,-1,0}}}; draw.transforms=gx::Shared<gx::Transforms>(t);
        gx::State s; s.viewport=std::array<float,6>{0,0,640,480,0,1}; draw.state=gx::Shared<gx::State>(s);
        gx::DrawUniform u{}; u.pixel[1]=17; draw.uniforms=gx::Shared<gx::DrawUniform>(u);
        draw.vertices.resize(3); draw.blend={1,4,5,3,true,true};
        const auto d=gx::summarize(draw);
        CHECK(d.callsite==0x80366E14 && d.perspective && d.z_test && d.z_write && d.vertices==3 && d.viewport && (*d.viewport)[2]==640);
        CHECK(d.blend_mode==1 && d.blend_source==4 && d.blend_destination==5);
        t.projection={{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}}; draw.transforms=gx::Shared<gx::Transforms>(t);
        u.pixel[1]=1; draw.uniforms=gx::Shared<gx::DrawUniform>(u);
        const auto o=gx::summarize(draw); CHECK(!o.perspective && o.z_test && !o.z_write);
    }
    {   // text round trip
        gx::PacketSummary p; p.present=false; p.copy_source=std::array<uint32_t,4>{0,0,320,240};
        p.draws.push_back({0x800421B8,false,false,false,std::nullopt,4,1,1,1});
        p.draws.push_back({0x80366E14,true,true,true,std::array<float,6>{0,0,640,480,0,1},120,0,1,0});
        gx::PacketSummary q; q.present=true;
        std::stringstream text; gx::write_packet(text,p); gx::write_packet(text,q);
        const auto back=gx::read_packets(text);
        CHECK(back.size()==2 && !back[0].present && back[0].copy_source==p.copy_source && back[0].draws.size()==2 && back[1].present && !back[1].copy_source && back[1].draws.empty());
        CHECK(back[0].draws[0].callsite==0x800421B8 && !back[0].draws[0].viewport && back[0].draws[0].blend_destination==1);
        CHECK(back[0].draws[1].perspective && back[0].draws[1].z_write && back[0].draws[1].viewport==p.draws[1].viewport && back[0].draws[1].vertices==120);
    }
    return 0;
}
