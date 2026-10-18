#include "scene_boundary.h"
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace gx {
DrawSummary summarize(const DrawPacket& draw) {
    DrawSummary d;
    d.callsite=draw.callsite;
    d.perspective=draw.transforms->projection[3][3]==0;   // GX perspective: w = -z; orthographic keeps w = 1
    const auto depth=draw.uniforms->pixel[1]&31;   // bit 0 compare enable, bit 4 update (pipeline key 68)
    d.z_test=depth&1; d.z_write=(depth&17)==17;
    d.viewport=draw.state->viewport;
    d.vertices=uint32_t(draw.vertex_view().size());
    d.blend_mode=draw.blend.mode; d.blend_source=draw.blend.source; d.blend_destination=draw.blend.destination;
    return d;
}
PacketSummary summarize(const FramePacket& packet) {
    PacketSummary p;
    p.draws.reserve(packet.draws.size());
    for(const auto& draw:packet.draws) p.draws.push_back(summarize(draw));
    if(packet.texture_copy) p.copy_source=packet.texture_copy->source;
    p.present=packet.present;
    return p;
}
void write_packet(std::ostream& out, const PacketSummary& packet) {
    out<<"P "<<packet.present<<' ';
    if(packet.copy_source) { const auto& c=*packet.copy_source; out<<c[0]<<','<<c[1]<<','<<c[2]<<','<<c[3]; } else out<<'-';
    out<<'\n';
    for(const auto& d:packet.draws) {
        out<<"D "<<std::hex<<d.callsite<<std::dec<<' '<<d.perspective<<' '<<d.z_test<<' '<<d.z_write<<' ';
        if(d.viewport) { for(size_t i=0;i<6;++i) out<<(i ? "," : "")<<(*d.viewport)[i]; } else out<<'-';
        out<<' '<<d.vertices<<' '<<unsigned(d.blend_mode)<<':'<<unsigned(d.blend_source)<<':'<<unsigned(d.blend_destination)<<'\n';
    }
}
void SceneBoundary::reset() { *this=SceneBoundary{}; }
std::optional<size_t> SceneBoundary::on_packet(const PacketSummary& packet) {
    std::optional<size_t> cut;
    if(!cut_) {
        for(const auto& draw:packet.draws) if(draw.perspective) { perspective_=true; viewport_=draw.viewport.value_or(std::array<float,6>{0,0,640,480,0,1}); }
        if(perspective_ && packet.draws.empty() && packet.copy_source && viewport_) {
            const auto& c=*packet.copy_source; const auto& v=*viewport_;
            if(c[0]<=v[0] && c[1]<=v[1] && c[0]+c[2]>=v[0]+v[2] && c[1]+c[3]>=v[1]+v[3]) { cut_=true; cut=0; }
        }
    }
    if(packet.present) { had_scene_=perspective_; perspective_=cut_=false; if(!had_scene_) viewport_.reset(); }
    else if(perspective_) had_scene_=true;
    return cut;
}
std::vector<PacketSummary> read_packets(std::istream& in) {
    std::vector<PacketSummary> packets;
    std::string line;
    while(std::getline(in,line)) {
        if(line.empty()) continue;
        std::istringstream s(line);
        std::string tag; s>>tag;
        if(tag=="P") {
            PacketSummary p; int present=0; std::string copy; s>>present>>copy;
            if(!s) throw std::runtime_error("scene summary: bad packet line: "+line);
            p.present=present!=0;
            if(copy!="-") { std::array<uint32_t,4> c{}; char comma; std::istringstream cs(copy); cs>>c[0]>>comma>>c[1]>>comma>>c[2]>>comma>>c[3]; if(!cs) throw std::runtime_error("scene summary: bad copy: "+line); p.copy_source=c; }
            packets.push_back(std::move(p));
        } else if(tag=="D") {
            if(packets.empty()) throw std::runtime_error("scene summary: draw before packet");
            DrawSummary d; int persp=0,ztest=0,zwrite=0; std::string viewport,blend;
            s>>std::hex>>d.callsite>>std::dec>>persp>>ztest>>zwrite>>viewport>>d.vertices>>blend;
            if(!s) throw std::runtime_error("scene summary: bad draw line: "+line);
            d.perspective=persp; d.z_test=ztest; d.z_write=zwrite;
            if(viewport!="-") { std::array<float,6> v{}; char comma; std::istringstream vs(viewport); vs>>v[0]; for(size_t i=1;i<6;++i) vs>>comma>>v[i]; if(!vs) throw std::runtime_error("scene summary: bad viewport: "+line); d.viewport=v; }
            unsigned m=0,src=0,dst=0; char colon; std::istringstream bs(blend); bs>>m>>colon>>src>>colon>>dst;
            if(!bs) throw std::runtime_error("scene summary: bad blend: "+line);
            d.blend_mode=uint8_t(m); d.blend_source=uint8_t(src); d.blend_destination=uint8_t(dst);
            packets.back().draws.push_back(d);
        } else throw std::runtime_error("scene summary: unknown line: "+line);
    }
    return packets;
}
}
