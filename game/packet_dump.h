#pragma once
#include "gx_packets.h"
#include "scene_boundary.h"
#include "scene_lights.h"
#include "ultrawide.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <cstdint>
namespace uc {
// Diagnostic: UC_PACKET_DUMP=<file>[|skip[|count]] lists every rendered packet (copy target, draws with callsite, textures, blend) of `count` presented frames after `skip` presents.
class PacketDump {
public:
    PacketDump() {
        const char* spec=std::getenv("UC_PACKET_DUMP");
        enable_stats();enable_motion();enable_scene();
        if(!spec) return;
        std::string text(spec);
        if(const auto bar=text.find('|'); bar!=std::string::npos) {   // path|skip|count
            if(text.compare(bar+1,2,"go")==0) go_file_=text.substr(0,bar)+".go";   // dump starts when this file appears
            else skip_=std::strtoul(text.c_str()+bar+1,nullptr,10);
            if(const auto second=text.find('|',bar+1); second!=std::string::npos) count_=std::strtoul(text.c_str()+second+1,nullptr,10);
            text.resize(bar);
        }
        file_=std::fopen(text.c_str(),"w");
    }
    // UC_PACKET_STATS=<file>: histogram of (callsite, blend, stages, texture size, copy target) over every rendered draw, written at exit.
    // UC_PACKET_MOTION=<file>|<frames> + <file>.go: one line per mesh draw of `frames` presented frames: frame, mesh pointer, vertices, hash of the position
    // matrices, hash of the uniform block (texture matrices, colours). Which draws change from frame to frame shows what the game animates.
    void enable_motion() {
        const char* spec=std::getenv("UC_PACKET_MOTION");
        if(!spec) return;
        std::string text(spec);
        if(const auto bar=text.find('|'); bar!=std::string::npos) {motion_frames_=std::strtoul(text.c_str()+bar+1,nullptr,10);text.resize(bar);}
        motion_file_=std::fopen(text.c_str(),"w");motion_go_=text+".go";
    }
    static uint64_t fnv(const void* data,size_t size,uint64_t hash=1469598103934665603ull) {
        const auto* bytes=static_cast<const unsigned char*>(data);
        for(size_t i=0;i<size;++i) hash=(hash^bytes[i])*1099511628211ull;
        return hash;
    }
    void motion(const gx::FramePacket& packet) {
        if(!motion_file_ || motion_done_) return;
        if(!motion_armed_) {if(!packet.present || !std::filesystem::exists(motion_go_)) return; motion_armed_=true;}
        for(const auto& draw:packet.draws) {
            if(draw.callsite!=0x80366E14) {   // other draws carry their own vertices: a changing vertex hash means moving positions or texture coordinates
                const auto view=draw.vertex_view();
                uint64_t hash=1469598103934665603ull;
                for(const auto& vertex:view) {hash=fnv(vertex.position.data(),sizeof vertex.position,hash);hash=fnv(vertex.texcoord.data(),2*sizeof vertex.texcoord[0],hash);}
                std::fprintf(motion_file_,"O %llu %08X %zu %p %llX",(unsigned long long)motion_frame_,draw.callsite,view.size(),(const void*)draw.texture.get(),(unsigned long long)hash);
                if(draw.texture&&!draw.texture->levels.empty()&&!draw.texture->gpu_only)std::fprintf(motion_file_," H %u %u %llX",draw.texture->levels[0].width,draw.texture->levels[0].height,(unsigned long long)fnv(draw.texture->levels[0].rgba.data(),draw.texture->levels[0].rgba.size()));
                std::fprintf(motion_file_," %llX %.5g %.5g %.5g\n",(unsigned long long)fnv(draw.uniforms->tex_rows.data(),sizeof draw.uniforms->tex_rows),
                    draw.uniforms->tex_rows[0][3],draw.uniforms->tex_rows[3][3],draw.uniforms->tex_rows[4][3]);
                continue;
            }
            const auto& rows=draw.transforms->position_rows;const auto at=draw.transforms->current<62 ? draw.transforms->current : 0;
            std::fprintf(motion_file_,"%llu %p %zu %llX %llX %u",(unsigned long long)motion_frame_,(const void*)draw.mesh.get(),draw.vertex_view().size(),
                (unsigned long long)fnv(rows.data(),sizeof rows),(unsigned long long)fnv(draw.uniforms->post_rows.data(),sizeof draw.uniforms->post_rows,fnv(draw.uniforms->tex_rows.data(),sizeof draw.uniforms->tex_rows)),draw.state->descriptors[0]!=gx::Mode::none);
            for(size_t r=0;r<3;++r)for(size_t c=0;c<4;++c)std::fprintf(motion_file_," %.6g",rows[at+r][c]);
            for(size_t r:{0,1,3,4})std::fprintf(motion_file_," %.6g",draw.uniforms->tex_rows[r][3]);
            std::fprintf(motion_file_," %llX %llX",(unsigned long long)fnv(draw.uniforms->indirect.data(),sizeof draw.uniforms->indirect,fnv(draw.uniforms->ind_cmd.data(),sizeof draw.uniforms->ind_cmd)),
                (unsigned long long)fnv(draw.uniforms->registers.data(),sizeof draw.uniforms->registers,fnv(draw.uniforms->konst.data(),sizeof draw.uniforms->konst)));
            std::fprintf(motion_file_," B %llX",(unsigned long long)(draw.texture_bank ? fnv(draw.texture_bank->rows.data(),sizeof draw.texture_bank->rows) : 0));
            {const auto& t=draw.texture; std::fprintf(motion_file_," T %p %llX",(const void*)t.get(),(unsigned long long)(t&&!t->levels.empty() ? fnv(t->levels[0].rgba.data(),t->levels[0].rgba.size()) : 0));}
            std::fprintf(motion_file_," %llX %llX",(unsigned long long)fnv(draw.uniforms->lights.data(),sizeof draw.uniforms->lights),
                (unsigned long long)fnv(draw.uniforms->materials.data(),sizeof draw.uniforms->materials,fnv(draw.uniforms->ambient.data(),sizeof draw.uniforms->ambient)));
            std::fputc('\n',motion_file_);
        }
        if(packet.present && ++motion_frame_>=motion_frames_) {motion_done_=true;std::fclose(motion_file_);motion_file_=nullptr;}
    }
    void enable_stats() {
        if(const char* path=std::getenv("UC_PACKET_STATS")) stats_path_=path;
        if(const char* path=std::getenv("UC_LIGHT_STATS")) lights_path_=path;
        if(const char* path=std::getenv("UC_MESH_STATS")) mesh_path_=path;
    }
    // UC_LIGHT_STATS=<file>: per callsite of the perspective (scene) draws, written at exit as CSV sorted by vertices: draws, vertices, draws with channel 0 / 1 lit,
    // lights used per kind (directional, point, spot, specular; counted per draw), vertices with a normal, blend, alpha-test draws, depth-writing draws, textured draws.
    struct LightRow { uint64_t draws=0,vertices=0,lit0=0,lit1=0,kinds[4]{},normals=0,alpha_test=0,depth_write=0,textured=0; std::string blend; };
    void light_stats(const gx::FramePacket& packet) {
        for(const auto& draw:packet.draws) {
            if(!gx::summarize(draw).perspective) continue;
            auto& row=lights_[draw.callsite];
            const auto view=draw.vertex_view();
            ++row.draws; row.vertices+=view.size();
            row.blend=std::to_string(draw.blend.mode)+":"+std::to_string(draw.blend.source)+":"+std::to_string(draw.blend.destination);
            for(const auto& vertex:view) row.normals+=vertex.normal[0]!=0 || vertex.normal[1]!=0 || vertex.normal[2]!=0;
            const auto& uniforms=*draw.uniforms;
            for(size_t channel=0;channel<2;++channel) {
                const auto lighting=gx::channel_lighting(uniforms.channels[channel]);
                if(!lighting.lit) continue;
                (channel ? row.lit1 : row.lit0)+=1;
                for(size_t light=0;light<8;++light) if(lighting.mask&(1u<<light)) ++row.kinds[size_t(gx::classify_light(uniforms.lights[light],lighting.attenuation))];
            }
            row.alpha_test+=((uniforms.pixel[0]>>16)&7)!=7 || ((uniforms.pixel[0]>>19)&7)!=7;   // an alpha compare other than "always"
            row.depth_write+=(uniforms.pixel[1]&16)!=0;
            row.textured+=draw.texture!=nullptr;
        }
    }
    // UC_MESH_STATS=<file>: tessellation survey (docs/spikes/S7-tessellation-survey.md). Per distinct mesh of the scene draws: triangles, positions shared by
    // vertices, and of those the ones whose vertex normals differ by more than 15 degrees (an edge there would crack when tessellated with per-vertex normals).
    // Written at exit: totals of drawn triangles per class (mesh path with matching normals, mesh path with diverging normals, inline vertices).
    struct MeshInfo { std::shared_ptr<const gx::MeshBuffers> keep; uint64_t triangles=0,shared=0,diverging=0,draws=0; };
    void mesh_stats(const gx::FramePacket& packet) {
        for(const auto& draw:packet.draws) {
            if(!gx::summarize(draw).perspective) continue;
            if(!draw.mesh) { inline_triangles_+=draw.index_view().empty() ? draw.vertex_view().size()/3 : draw.index_view().size()/3; continue; }
            auto& info=meshes_[draw.mesh.get()];
            if(!info.keep) {
                info.keep=draw.mesh; info.triangles=draw.mesh->indices.size()/3;
                std::map<std::array<int,3>,std::vector<std::array<float,3>>> positions;
                for(const auto& v:draw.mesh->vertices) {
                    const std::array<int,3> key{int(std::lround(v.position[0]*1000)),int(std::lround(v.position[1]*1000)),int(std::lround(v.position[2]*1000))};
                    const float length=std::sqrt(v.normal[0]*v.normal[0]+v.normal[1]*v.normal[1]+v.normal[2]*v.normal[2]);
                    positions[key].push_back(length>0 ? std::array<float,3>{v.normal[0]/length,v.normal[1]/length,v.normal[2]/length} : std::array<float,3>{0,0,0});
                }
                for(const auto& [key,normals]:positions) {
                    if(normals.size()<2) continue;
                    ++info.shared;
                    for(const auto& n:normals) if(n[0]*normals[0][0]+n[1]*normals[0][1]+n[2]*normals[0][2]<0.9659f) {++info.diverging;break;}   // cos 15 degrees
                }
            }
            ++info.draws;
            (info.diverging*20>info.shared ? drawn_diverging_ : drawn_matching_)+=info.triangles;   // diverging: more than 5% of the shared positions
        }
    }
    // UC_SCENE_DUMP=<file>|<every>|<frames>: every `every`-th presented frame (up to `frames` of them) is written as scene summaries
    // (renderer/scene_boundary.h text) after an "F <presented index> <serial of the presenting packet>" line; scene_sampled() tells
    // the caller to capture that frame, so each summary has a screenshot (frame-...-<serial>.png).
    void enable_scene() {
        const char* spec=std::getenv("UC_SCENE_DUMP");
        if(!spec) return;
        std::string text(spec);
        if(const auto bar=text.find('|'); bar!=std::string::npos) {
            scene_every_=std::max(1ul,std::strtoul(text.c_str()+bar+1,nullptr,10));
            if(const auto second=text.find('|',bar+1); second!=std::string::npos) scene_frames_=std::strtoul(text.c_str()+second+1,nullptr,10);
            text.resize(bar);
        }
        scene_file_.open(text);
    }
    void scene(const gx::FramePacket& packet) {
        scene_sampled_=false;
        if(!scene_file_.is_open() || scene_written_>=scene_frames_) return;
        if(scene_presented_%scene_every_==0) {
            scene_frame_.push_back(gx::summarize(packet));
            if(packet.present) {
                scene_file_<<"F "<<scene_presented_<<' '<<packet.serial<<'\n';
                for(const auto& summary:scene_frame_) gx::write_packet(scene_file_,summary);
                scene_file_.flush(); scene_sampled_=true; ++scene_written_;
            }
        }
        if(packet.present) { ++scene_presented_; scene_frame_.clear(); }
    }
    bool scene_sampled() const { return scene_sampled_; }
    ~PacketDump() {
        if(file_) std::fclose(file_);
        if(!lights_path_.empty()) if(std::FILE* out=std::fopen(lights_path_.c_str(),"w")) {
            std::vector<std::pair<uint32_t,LightRow>> rows(lights_.begin(),lights_.end());
            std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b) { return a.second.vertices>b.second.vertices; });
            std::fprintf(out,"callsite,draws,vertices,lit0,lit1,directional,point,spot,specular,normals,blend,alpha_test,depth_write,textured\n");
            for(const auto& [site,r]:rows)
                std::fprintf(out,"%08X,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%s,%llu,%llu,%llu\n",site,(unsigned long long)r.draws,(unsigned long long)r.vertices,(unsigned long long)r.lit0,(unsigned long long)r.lit1,
                    (unsigned long long)r.kinds[0],(unsigned long long)r.kinds[1],(unsigned long long)r.kinds[2],(unsigned long long)r.kinds[3],(unsigned long long)r.normals,r.blend.c_str(),
                    (unsigned long long)r.alpha_test,(unsigned long long)r.depth_write,(unsigned long long)r.textured);
            std::fclose(out);
        }
        if(!mesh_path_.empty()) if(std::FILE* out=std::fopen(mesh_path_.c_str(),"w")) {
            uint64_t shared=0,diverging=0,bad_meshes=0;
            for(const auto& [pointer,m]:meshes_) {shared+=m.shared;diverging+=m.diverging;bad_meshes+=m.diverging*20>m.shared;}
            const double total=double(drawn_matching_+drawn_diverging_+inline_triangles_);
            std::fprintf(out,"distinct meshes %zu (diverging %llu), shared positions %llu, diverging positions %llu\n",meshes_.size(),(unsigned long long)bad_meshes,(unsigned long long)shared,(unsigned long long)diverging);
            std::fprintf(out,"drawn triangles: matching-normal meshes %llu, diverging meshes %llu, inline vertices %llu; matching share %.1f%%\n",(unsigned long long)drawn_matching_,(unsigned long long)drawn_diverging_,
                (unsigned long long)inline_triangles_,total>0 ? 100.0*double(drawn_matching_)/total : 0.0);
            std::fclose(out);
        }
        if(!stats_path_.empty()) if(std::FILE* out=std::fopen(stats_path_.c_str(),"w")) {
            for(const auto& [key,count]:stats_) {
                const auto& [site,blend,stages,texture,copy]=key;
                std::fprintf(out,"%8llu site %08X blend %s stages %u tex %s copy %s\n",(unsigned long long)count,site,blend.c_str(),stages,texture.c_str(),copy.c_str());
            }
            std::fclose(out);
        }
    }
    PacketDump(const PacketDump&)=delete;
    PacketDump& operator=(const PacketDump&)=delete;
    void packet(const gx::FramePacket& packet) {
        motion(packet);
        scene(packet);
        if(!lights_path_.empty()) light_stats(packet);
        if(!mesh_path_.empty()) mesh_stats(packet);
        if(!stats_path_.empty()) {
            std::string copy="-";
            if(packet.texture_copy) copy=std::to_string(packet.texture_copy->target->levels[0].width)+"x"+std::to_string(packet.texture_copy->target->levels[0].height);
            for(const auto& draw:packet.draws) {
                std::string texture="-";
                if(draw.texture && !draw.texture->levels.empty()) texture=std::to_string(draw.texture->levels[0].width)+"x"+std::to_string(draw.texture->levels[0].height);
                {bool tmi=false; for(size_t d=1;d<9;++d) tmi|=draw.state->descriptors[d]!=gx::Mode::none; if(tmi) texture+="+TMI";}
                if(draw.texture && !draw.texture->levels.empty()) { char sampler[48]; std::snprintf(sampler,sizeof sampler," L%zu wrap%02X",draw.texture->levels.size(),unsigned(draw.texture_wrap[0])); texture+=sampler; }   // mip levels and the GX sampler mode (bits: wrap s/t 0-3, mip 5-6, mag 4, min 7)
                const std::string blend=std::to_string(draw.blend.mode)+":"+std::to_string(draw.blend.source)+":"+std::to_string(draw.blend.destination);
                ++stats_[{draw.callsite,blend,draw.material.stages,texture,copy}];
            }
        }
        if(file_ && !go_file_.empty() && !armed_) {
            if(!packet.present || !std::filesystem::exists(go_file_)) return;
            armed_=true; skip_=frame_;
        }
        if(!file_ || frame_<skip_ || frame_>=skip_+count_) { if(file_ && packet.present) ++frame_; return; }
        std::fprintf(file_,"packet %llu frame %llu present %u draws %zu",(unsigned long long)packet.serial,(unsigned long long)frame_,packet.present,packet.draws.size());
        if(packet.texture_copy) {
            const auto& copy=*packet.texture_copy;
            std::fprintf(file_," copy src %u:%u:%u:%u target %ux%u@%p",copy.source[0],copy.source[1],copy.source[2],copy.source[3],copy.target->levels[0].width,copy.target->levels[0].height,(const void*)copy.target.get());
        }
        std::fprintf(file_,"\n");
        size_t index=0;
        for(const auto& draw:packet.draws) {
            std::fprintf(file_,"  draw %zu site %08X prim %02X verts %zu stages %u blend %u:%u:%u tex",index++,draw.callsite,draw.primitive,draw.vertex_view().size(),draw.material.stages,draw.blend.mode,draw.blend.source,draw.blend.destination);
            for(size_t slot=0;slot<draw.texture_maps.size();++slot) { const auto& map=draw.texture_maps[slot]; if(map && !map->levels.empty()) std::fprintf(file_," map%zu=%ux%u%s@%p",slot,map->levels[0].width,map->levels[0].height,map->guest_address ? "(copy)" : "",(const void*)map.get()); }
            for(uint32_t stage=0;stage<draw.material.stages && stage<16;++stage) std::fprintf(file_," s%u:%03X",stage,draw.material.stage[stage].order&0x7F);
            if(draw.texture && !draw.texture->levels.empty()) std::fprintf(file_," t0=%ux%u@%p",draw.texture->levels[0].width,draw.texture->levels[0].height,(const void*)draw.texture.get());
            if(draw.texture && draw.texture->guest_address) std::fprintf(file_," copy@%X",draw.texture->guest_address);
            if(draw.reticle||draw.late_aim||draw.wake||draw.layer) std::fprintf(file_," flags[%s%s%s%s]",draw.reticle?"R":"",draw.late_aim?"L":"",draw.wake?"W":"",draw.layer?"Y":"");
            if(gx::is_perspective(draw.transforms->projection)) std::fprintf(file_," persp");
            else if(const auto range=gx::ortho_range(draw,0)) { std::fprintf(file_," ortho x %.3f..%.3f",(*range)[0],(*range)[1]); if(const auto y=gx::ortho_range(draw,1)) std::fprintf(file_," y %.3f..%.3f",(*y)[0],(*y)[1]); }   // after the ultrawide narrowing
            std::fprintf(file_,"\n");
        }
        std::fflush(file_);
        if(packet.present) ++frame_;
    }
private:
    std::FILE* file_=nullptr;
    unsigned long skip_=0,count_=3;
    std::string go_file_,stats_path_,motion_go_,lights_path_;
    std::map<uint32_t,LightRow> lights_;
    std::string mesh_path_;
    std::map<const void*,MeshInfo> meshes_;
    uint64_t drawn_matching_=0,drawn_diverging_=0,inline_triangles_=0;
    std::FILE* motion_file_=nullptr;
    unsigned long motion_frames_=300;
    unsigned long long motion_frame_=0;
    bool motion_armed_=false,motion_done_=false;
    std::map<std::tuple<uint32_t,std::string,uint32_t,std::string,std::string>,uint64_t> stats_;
    bool armed_=false;
    unsigned long long frame_=0;
    std::ofstream scene_file_;
    std::vector<gx::PacketSummary> scene_frame_;
    unsigned long scene_every_=600,scene_frames_=100,scene_written_=0;
    unsigned long long scene_presented_=0;
    bool scene_sampled_=false;
};
}
