#include <algorithm>
#include <string>
#include <vector>
#include <set>
#include <map>
#include "gx_stream.h"
#include "image_compare.h"
#include "vulkan_renderer.h"
#include <Windows.h>
#include <charconv>
#include <chrono>
#include <cstdio>

int main(int argc,char** argv) {
    SDL_Window* window=nullptr;
    try {
        if(argc<4)throw std::runtime_error("usage: uc_fifo_replay capture.dff frame output.png [--validation] [--uber] [--reference image.png] [--dump-efb dir] [--allow-unresolved]");
        bool validation=false,uber=false,allow_unresolved=false,anim_report=false;std::filesystem::path reference_path,efb_dump;
        for(int i=4;i<argc;++i) {
            if(std::string_view(argv[i])=="--validation")validation=true;
            else if(std::string_view(argv[i])=="--uber")uber=true;
            else if(std::string_view(argv[i])=="--anim-report")anim_report=true;
            else if(std::string_view(argv[i])=="--allow-unresolved")allow_unresolved=true;
            else if(std::string_view(argv[i])=="--reference" && i+1<argc)reference_path=argv[++i];
            else if(std::string_view(argv[i])=="--dump-efb" && i+1<argc)efb_dump=argv[++i];
            else throw std::runtime_error("unknown or incomplete option");
        }
        std::optional<uc::RenderPixels> reference;
        auto output=std::filesystem::path(argv[3]);auto diff=output.parent_path()/(output.stem().string()+"-diff.png");
        if(!reference_path.empty()) {
            auto canonical=std::filesystem::weakly_canonical(reference_path);
            if(canonical==std::filesystem::weakly_canonical(output) || canonical==std::filesystem::weakly_canonical(diff) ||
               (std::filesystem::exists(output) && std::filesystem::equivalent(output,reference_path)) ||
               (std::filesystem::exists(diff) && std::filesystem::equivalent(diff,reference_path)))
                throw std::runtime_error("output must not overwrite reference image");
            reference=fifo_replay::load_reference(reference_path);
            if(reference->width!=640 || reference->height!=480)throw std::runtime_error("reference must be 640x480; automatic resizing would hide comparison errors");
        }
        size_t frame=0;std::string_view number=argv[2];auto parsed=std::from_chars(number.data(),number.data()+number.size(),frame);
        if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size())throw std::runtime_error("invalid frame number");
        auto capture=fifo_replay::load_dff(argv[1]);fifo_replay::GxStream stream(capture,frame);stream.allow_unresolved=allow_unresolved;auto packets=stream.packets();
        std::fprintf(stderr,"replay: %zu packets, %llu draws, %llu vertices\n",packets.size(),stream.stats.draws,stream.stats.vertices);
        if(anim_report) { // which draws (same object-space geometry) change texture image or texture matrices between frames
            struct Seen { std::set<uint64_t> textures,rows; unsigned count=0; uint32_t w=0,h=0; uint32_t tmi=0; };
            std::map<uint64_t,Seen> seen;
            auto fnv=[](const void* data,size_t n,uint64_t h=1469598103934665603ull){auto* b=static_cast<const unsigned char*>(data);for(size_t i=0;i<n;++i)h=(h^b[i])*1099511628211ull;return h;};
            for(const auto& packet:packets)for(const auto& draw:packet.draws) {
                uint64_t key=1469598103934665603ull;
                for(const auto& v:draw.vertex_view()){key=fnv(v.position.data(),12,key);key=fnv(v.texcoord[0].data(),8,key);}
                auto& e=seen[key];++e.count;
                uint64_t t=draw.texture&&!draw.texture->levels.empty()?fnv(draw.texture->levels[0].rgba.data(),draw.texture->levels[0].rgba.size()):0;
                e.textures.insert(t);if(draw.texture&&!draw.texture->levels.empty()){e.w=draw.texture->levels[0].width;e.h=draw.texture->levels[0].height;}
                e.rows.insert(fnv(draw.uniforms->tex_rows.data(),sizeof draw.uniforms->tex_rows));
                e.tmi|=gx::has_texture_matrix_index(*draw.state);
                static unsigned shown=0;
                if(e.rows.size()==2&&e.count>1&&shown<12){++shown;
                    std::fprintf(stderr,"anim: moving matrix geometry verts %zu tex %ux%u stages %u blend %u:%u:%u gen0 %05X gen1 %05X rows0 [%.3f %.3f %.3f %.3f][%.3f %.3f %.3f %.3f] rows3 [%.3f %.3f %.3f %.3f]\n",draw.vertex_view().size(),e.w,e.h,draw.material.stages,draw.blend.mode,draw.blend.source,draw.blend.destination,draw.uniforms->tex_gens[0],draw.uniforms->tex_gens[1],
                        draw.uniforms->tex_rows[0][0],draw.uniforms->tex_rows[0][1],draw.uniforms->tex_rows[0][2],draw.uniforms->tex_rows[0][3],draw.uniforms->tex_rows[1][0],draw.uniforms->tex_rows[1][1],draw.uniforms->tex_rows[1][2],draw.uniforms->tex_rows[1][3],draw.uniforms->tex_rows[3][0],draw.uniforms->tex_rows[3][1],draw.uniforms->tex_rows[3][2],draw.uniforms->tex_rows[3][3]);}
            }
            unsigned repeated=0,texture_changes=0,row_changes=0,both=0;std::map<std::pair<uint32_t,uint32_t>,unsigned> sizes;
            for(const auto& [k,e]:seen) if(e.count>1){++repeated;bool tc=e.textures.size()>1,rc=e.rows.size()>1;texture_changes+=tc;row_changes+=rc;both+=tc&&rc;if(tc)++sizes[{e.w,e.h}];}
            std::fprintf(stderr,"anim: %zu distinct geometries, %u repeated across draws, texture image varies %u, texture matrices vary %u (both %u)\n",seen.size(),repeated,texture_changes,row_changes,both);
            {std::map<uint32_t,uint64_t> sources;uint64_t draws=0;
                for(const auto& packet:packets)for(const auto& draw:packet.draws){++draws;for(size_t u=0;u<draw.material.stages&&u<8;++u)(void)u;std::set<uint32_t> used;for(size_t u=0;u<8;++u)used.insert((draw.uniforms->tex_gens[u]>>7)&31);for(auto s:used)++sources[s];}
                std::fprintf(stderr,"anim: %llu draws; texgen source kinds used by draws:",(unsigned long long)draws);for(auto&[s,n]:sources)std::fprintf(stderr," src%u:%llu",s,(unsigned long long)n);std::fprintf(stderr,"\n");}
            {std::map<uint32_t,uint64_t> words;for(const auto& packet:packets)for(const auto& draw:packet.draws)for(size_t u=0;u<8;++u)++words[(u<<24)|draw.uniforms->tex_gens[u]];
                std::fprintf(stderr,"anim: texgen words (unit:word x draws):");for(auto&[w,n]:words)std::fprintf(stderr," %u:%05X x%llu",w>>24,w&0xFFFFFF,(unsigned long long)n);std::fprintf(stderr,"\n");}
            {std::map<std::string,uint64_t> signatures;
                for(const auto& packet:packets)for(const auto& draw:packet.draws){char text[160];int n=std::snprintf(text,sizeof text,"stages %u gen1 %05X orders",draw.material.stages,draw.uniforms->tex_gens[1]);
                    for(uint32_t s=0;s<draw.material.stages&&s<4;++s)n+=std::snprintf(text+n,sizeof text-size_t(n)," %03X",draw.material.stage[s].order);++signatures[text];}
                std::vector<std::pair<uint64_t,std::string>> ranked;for(auto&[k,v]:signatures)ranked.push_back({v,k});std::sort(ranked.rbegin(),ranked.rend());
                for(size_t i=0;i<ranked.size()&&i<12;++i)std::fprintf(stderr,"anim: signature x%llu %s\n",(unsigned long long)ranked[i].first,ranked[i].second.c_str());}
            {uint64_t normal_gen=0,normal_gen_with_uv1=0,uv_gen=0,uv_gen_with_uv1=0;
                for(const auto& packet:packets)for(const auto& draw:packet.draws){const bool has_uv1=draw.state->descriptors[14]!=gx::Mode::none;const bool normal=((draw.uniforms->tex_gens[1]>>7)&31)==1;(normal?normal_gen:uv_gen)++;if(has_uv1)(normal?normal_gen_with_uv1:uv_gen_with_uv1)++;}
                std::fprintf(stderr,"anim: unit1 texgen from normal: %llu draws (%llu have a TEX1 vertex attribute); from TEX1: %llu draws (%llu have it)\n",(unsigned long long)normal_gen,(unsigned long long)normal_gen_with_uv1,(unsigned long long)uv_gen,(unsigned long long)uv_gen_with_uv1);}
            {struct Uv { std::set<uint64_t> variants; unsigned count=0; uint32_t w=0,h=0; unsigned stages=0; };
                std::map<uint64_t,Uv> by_position;
                for(const auto& packet:packets)for(const auto& draw:packet.draws){
                    uint64_t pk=1469598103934665603ull,uk=1469598103934665603ull;
                    for(const auto& v:draw.vertex_view()){pk=fnv(v.position.data(),12,pk);uk=fnv(v.texcoord[0].data(),8,uk);uk=fnv(v.texcoord[1].data(),8,uk);}
                    auto& e=by_position[pk];e.variants.insert(uk);++e.count;e.stages=draw.material.stages;
                    if(draw.texture&&!draw.texture->levels.empty()){e.w=draw.texture->levels[0].width;e.h=draw.texture->levels[0].height;}
                }
                unsigned repeated=0,varying=0;std::map<std::pair<uint32_t,uint32_t>,unsigned> sizes;
                for(const auto& [k,e]:by_position)if(e.count>1){++repeated;if(e.variants.size()>1){++varying;++sizes[{e.w,e.h}];}}
                std::fprintf(stderr,"anim: %u geometries repeat; texture coordinates (UV0/UV1) vary for %u of them\n",repeated,varying);
                for(const auto& [dim,n]:sizes)std::fprintf(stderr,"anim: UV-varying geometries with a %ux%u first texture: %u\n",dim.first,dim.second,n);}
            {std::map<std::string,uint64_t> viewports;
                for(const auto& packet:packets)for(const auto& draw:packet.draws){char text[160];if(draw.state->viewport){const auto& v=*draw.state->viewport;std::snprintf(text,sizeof text,"%.1f %.1f %.1f %.1f %.2f %.2f",v[0],v[1],v[2],v[3],v[4],v[5]);}else std::snprintf(text,sizeof text,"none");++viewports[text];}
                for(const auto& [k,n]:viewports)std::fprintf(stderr,"anim: viewport %s x%llu\n",k.c_str(),(unsigned long long)n);}
            {size_t index=0;for(const auto& packet:packets){std::fprintf(stderr,"anim: packet %zu draws %zu present %d clear %d copy %s",index++,packet.draws.size(),int(packet.present),int(packet.clear_after_copy),packet.texture_copy?"":"-");
                if(packet.texture_copy)std::fprintf(stderr,"%ux%u src %u:%u:%u:%u",packet.texture_copy->target->levels[0].width,packet.texture_copy->target->levels[0].height,packet.texture_copy->source[0],packet.texture_copy->source[1],packet.texture_copy->source[2],packet.texture_copy->source[3]);
                std::fprintf(stderr,"\n");}}
            if(packets.size()>34 && packets[0].draws.size()==packets[17].draws.size()) for(size_t i:{100,5000,9000})for(size_t f:{0,17,34}){ const auto& d=packets[f].draws[i];
                std::fprintf(stderr,"anim: draw %zu packet %zu posts %03X %03X %03X: unit0 post [%.3f %.3f %.3f %.3f][%.3f %.3f %.3f %.3f][%.3f %.3f %.3f %.3f] unit1 post [%.3f %.3f %.3f %.3f][%.3f %.3f %.3f %.3f]\n",i,f,d.uniforms->tex_posts[0],d.uniforms->tex_posts[1],d.uniforms->tex_posts[2],
                    d.uniforms->post_rows[0][0],d.uniforms->post_rows[0][1],d.uniforms->post_rows[0][2],d.uniforms->post_rows[0][3],d.uniforms->post_rows[1][0],d.uniforms->post_rows[1][1],d.uniforms->post_rows[1][2],d.uniforms->post_rows[1][3],d.uniforms->post_rows[2][0],d.uniforms->post_rows[2][1],d.uniforms->post_rows[2][2],d.uniforms->post_rows[2][3],
                    d.uniforms->post_rows[3][0],d.uniforms->post_rows[3][1],d.uniforms->post_rows[3][2],d.uniforms->post_rows[3][3],d.uniforms->post_rows[4][0],d.uniforms->post_rows[4][1],d.uniforms->post_rows[4][2],d.uniforms->post_rows[4][3]);}
            if(packets.size()>34 && packets[0].draws.size()==packets[17].draws.size()){ // frames recorded with identical draw sequences: compare draw i of frame 0 with draw i of frame 1
                const auto& a=packets[0].draws;const auto& b=packets[17].draws;
                struct Counts{uint64_t n=0;uint64_t pos=0,uv=0,color=0,rows=0,post=0,regs=0,light=0,tex=0,matrix=0,state=0;};
                std::map<std::string,Counts> classes;
                for(size_t i=0;i<a.size();++i){
                    const auto& x=a[i];const auto& y=b[i];
                    auto geo=[&](const gx::DrawPacket& d,int what){uint64_t h=1469598103934665603ull;for(const auto& v:d.vertex_view()){if(what==0)h=fnv(v.position.data(),12,h);else if(what==1){h=fnv(v.texcoord[0].data(),8,h);h=fnv(v.texcoord[1].data(),8,h);}else h=fnv(v.color[0].data(),4,fnv(v.color[1].data(),4,h));}return h;};
                    auto texhash=[&](const gx::DrawPacket& d){return d.texture&&!d.texture->levels.empty()?fnv(d.texture->levels[0].rgba.data(),d.texture->levels[0].rgba.size()):uint64_t(d.texture&&d.texture->gpu_only);};
                    char name[96];std::snprintf(name,sizeof name,"stages %u tex %ux%u%s gen1 %05X",x.material.stages,x.texture&&!x.texture->levels.empty()?x.texture->levels[0].width:0,x.texture&&!x.texture->levels.empty()?x.texture->levels[0].height:0,x.texture&&x.texture->gpu_only?" EFB":"",x.uniforms->tex_gens[1]);
                    auto& c=classes[name];++c.n;
                    c.pos+=geo(x,0)!=geo(y,0);c.uv+=geo(x,1)!=geo(y,1);c.color+=geo(x,2)!=geo(y,2);
                    c.rows+=fnv(x.uniforms->tex_rows.data(),sizeof x.uniforms->tex_rows)!=fnv(y.uniforms->tex_rows.data(),sizeof y.uniforms->tex_rows);
                    c.post+=fnv(x.uniforms->post_rows.data(),sizeof x.uniforms->post_rows)!=fnv(y.uniforms->post_rows.data(),sizeof y.uniforms->post_rows);
                    c.regs+=fnv(x.uniforms->registers.data(),sizeof x.uniforms->registers,fnv(x.uniforms->konst.data(),sizeof x.uniforms->konst))!=fnv(y.uniforms->registers.data(),sizeof y.uniforms->registers,fnv(y.uniforms->konst.data(),sizeof y.uniforms->konst));
                    c.light+=fnv(x.uniforms->lights.data(),sizeof x.uniforms->lights,fnv(x.uniforms->materials.data(),sizeof x.uniforms->materials))!=fnv(y.uniforms->lights.data(),sizeof y.uniforms->lights,fnv(y.uniforms->materials.data(),sizeof y.uniforms->materials));
                    c.tex+=texhash(x)!=texhash(y);
                    c.matrix+=fnv(x.transforms->position_rows.data(),sizeof x.transforms->position_rows)!=fnv(y.transforms->position_rows.data(),sizeof y.transforms->position_rows);
                }
                for(const auto& [k,c]:classes)if(c.n>=20)std::fprintf(stderr,"anim: frame0->1 [%s] draws %llu: position-matrix %llu uv %llu vertex-pos %llu color %llu texrows %llu postrows %llu regs %llu lights %llu texture %llu\n",k.c_str(),(unsigned long long)c.n,(unsigned long long)c.matrix,(unsigned long long)c.uv,(unsigned long long)c.pos,(unsigned long long)c.color,(unsigned long long)c.rows,(unsigned long long)c.post,(unsigned long long)c.regs,(unsigned long long)c.light,(unsigned long long)c.tex);
            }
            if(const char* path=std::getenv("UC_REPLAY_TEXTURES")) { // unique (size, hash) of every decoded texture, with the number of draws and the smallest vertex count
                std::map<std::string,std::pair<uint64_t,size_t>> textures;
                for(const auto& packet:packets)for(const auto& draw:packet.draws){ if(!draw.texture||draw.texture->levels.empty()||draw.texture->gpu_only)continue;
                    char text[96];std::snprintf(text,sizeof text,"%u %u %llX",draw.texture->levels[0].width,draw.texture->levels[0].height,(unsigned long long)fnv(draw.texture->levels[0].rgba.data(),draw.texture->levels[0].rgba.size()));
                    auto& e=textures.try_emplace(text,0,SIZE_MAX).first->second;++e.first;e.second=std::min(e.second,draw.vertex_view().size());}
                if(FILE* out=std::fopen(path,"w")){for(const auto& [k,e]:textures)std::fprintf(out,"%s draws %llu minverts %zu\n",k.c_str(),(unsigned long long)e.first,e.second);std::fclose(out);}
            }
            unsigned row_tmi=0;for(const auto& [k,e]:seen)if(e.count>1&&e.rows.size()>1&&e.tmi)++row_tmi;
            std::fprintf(stderr,"anim: of the geometries whose texture matrices vary, %u use per-vertex matrix indices\n",row_tmi);
            for(const auto& [dim,n]:sizes)std::fprintf(stderr,"anim: texture-varying geometries with a %ux%u first texture: %u\n",dim.first,dim.second,n);
            return 0;
        }
        if(const char* only=std::getenv("UC_REPLAY_ONLY_ANIM")) { // render only the draws whose texture matrices (1) or texture image (2) change between frames, everything else masked
            const bool by_rows=std::string_view(only)!="2";
            auto fnv=[](const void* data,size_t n,uint64_t h=1469598103934665603ull){auto* b=static_cast<const unsigned char*>(data);for(size_t i=0;i<n;++i)h=(h^b[i])*1099511628211ull;return h;};
            std::map<uint64_t,std::set<uint64_t>> variants;
            auto key_of=[&](const gx::DrawPacket& d){uint64_t k=1469598103934665603ull;for(const auto& v:d.vertex_view()){k=fnv(v.position.data(),12,k);k=fnv(v.texcoord[0].data(),8,k);}return k;};
            auto value_of=[&](const gx::DrawPacket& d){return by_rows ? fnv(d.uniforms->tex_rows.data(),sizeof d.uniforms->tex_rows) : (d.texture&&!d.texture->levels.empty() ? fnv(d.texture->levels[0].rgba.data(),d.texture->levels[0].rgba.size()) : 0);};
            for(auto& packet:packets)for(auto& draw:packet.draws)variants[key_of(draw)].insert(value_of(draw));
            uint64_t kept=0;
            for(auto& packet:packets)for(auto& draw:packet.draws){if(variants[key_of(draw)].size()>1)++kept;else {draw.blend.color_write=false;draw.blend.alpha_write=false;}}
            std::fprintf(stderr,"replay: UC_REPLAY_ONLY_ANIM keeps %llu draws\n",(unsigned long long)kept);
        }
        if(!SDL_Init(SDL_INIT_VIDEO))throw std::runtime_error(SDL_GetError());
        window=SDL_CreateWindow("Umbrella Chronicles — FIFO replay",640,480,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
        if(!window)throw std::runtime_error(SDL_GetError());
        auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
        if(!hwnd)throw std::runtime_error("replay window has no native handle");
        ShowWindow(hwnd,SW_SHOWNOACTIVATE);
        {
            uc::VulkanRenderer renderer;renderer.initialize(window,validation,std::filesystem::path(SDL_GetBasePath())/"data/shaders",2.0/3,4.0/3);renderer.set_uber_only(uber);
            uc::RenderPixels pixels;
            for(const auto& packet:packets) {
                bool submitted=false,completed=false;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
                while(!completed) {
                    if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("GPU replay timed out");
                    SDL_Event event;while(SDL_PollEvent(&event))if(event.type==SDL_EVENT_QUIT)throw std::runtime_error("replay canceled");
                    if(!submitted)submitted=renderer.render(packet);
                    if(submitted)renderer.wait_for_pending(1000000);
                    for(auto serial:renderer.poll_completions())if(serial==packet.serial)completed=true;
                    if(!completed)SDL_Delay(1);
                }
                if(packet.present)pixels=renderer.capture(packet.serial);
                // Diagnostics: the EFB each texture copy read, for stage-by-stage comparison with Dolphin's EFB copy dumps.
                if(!efb_dump.empty() && packet.texture_copy) {
                    std::filesystem::create_directories(efb_dump);
                    uc::save_png(renderer.capture(packet.serial,true),efb_dump/("efb-"+std::to_string(packet.serial)+".png"));
                }
            }
            if(renderer.validation_errors())throw std::runtime_error("Vulkan validation errors");
            if(pixels.rgba.empty())throw std::runtime_error("capture has no displayed frame");
            uc::save_png(pixels,output);
            if(reference) {
                auto comparison=fifo_replay::compare_images(pixels,*reference);uc::save_png(comparison.diff,diff);
                std::fprintf(stderr,"RGB absolute difference: mean %.6f/255, peak %u/255, changed pixels %llu/%llu; %s\n",
                    comparison.mean,comparison.peak,comparison.changed_pixels,uint64_t(pixels.width)*pixels.height,diff.string().c_str());
            }
            renderer.shutdown();
        }
        SDL_DestroyWindow(window);SDL_Quit();std::fprintf(stderr,"saved %s\n",argv[3]);return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"replay: %s\n",e.what());if(window)SDL_DestroyWindow(window);SDL_Quit();return 1;
    }
}
