#include "check.h"
#include "game_renderer.h"
#include "rebuild_gate.h"
#include <intrin.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using Vec=std::array<float,3>;
using Quad=std::array<Vec,4>;
gx::DrawPacket quad(const Quad& corners,Vec normal,std::array<uint8_t,4> color,bool perspective=true) {
    gx::DrawPacket draw; draw.callsite=0x72740000; draw.primitive=0x90; draw.indices={0,1,2,0,2,3};
    for (const auto& corner : corners) { gx::Vertex vertex; vertex.position=corner; vertex.normal={normal[0],normal[1],normal[2],0,0,0,0,0,0}; vertex.color[0]=color; draw.vertices.push_back(vertex); }
    draw.state.edit().viewport=std::array<float,6>{0,0,640,360,0,1};
    draw.transforms.edit().projection=perspective ? gx::Projection{{{1,0,0,0},{0,1,0,0},{0,0,-100.f/99,-100.f/99},{0,0,-1,0}}} :
        gx::Projection{{{1,0,0,0},{0,1,0,0},{0,0,0,.5f},{0,0,0,1}}};
    draw.uniforms.edit().pixel[1]=perspective ? 1u|(3u<<1)|(1u<<4) : 0;
    return draw;
}
std::shared_ptr<const gx::MeshBuffers> mesh(const Quad& corners,Vec normal,std::array<uint8_t,4> color) {
    auto draw=quad(corners,normal,color); return std::make_shared<const gx::MeshBuffers>(gx::MeshBuffers{std::move(draw.vertices),std::move(draw.indices)});
}
int channel(const uc::RenderPixels& picture,uint32_t x,uint32_t y,size_t c) { return picture.rgba[(size_t(y)*picture.width+x)*4+c]; }
void hud_unchanged(const uc::RenderPixels& picture) {
    // Interior of the orthographic red HUD quad: all RT passes finish before this draw.
    for (uint32_t y=picture.height*9/100;y<picture.height*16/100;++y) for (uint32_t x=picture.width*84/100;x<picture.width*91/100;++x)
        CHECK(channel(picture,x,y,0)==255 && channel(picture,x,y,1)==0 && channel(picture,x,y,2)==0);
}
struct Difference { size_t darker=0,brighter_red=0,changed=0; int largest=0; };
Difference floor_difference(const uc::RenderPixels& base,const uc::RenderPixels& changed) {
    CHECK(base.width==changed.width && base.height==changed.height);
    Difference result;
    // Visible floor: above its horizon in the renderer's positive-down GX viewport.
    for (uint32_t y=base.height*27/100;y<base.height*41/100;++y) for (uint32_t x=base.width*15/100;x<base.width*85/100;++x) {
        const int red=channel(changed,x,y,0)-channel(base,x,y,0),green=channel(changed,x,y,1)-channel(base,x,y,1),blue=channel(changed,x,y,2)-channel(base,x,y,2);
        const int largest=std::max({std::abs(red),std::abs(green),std::abs(blue)});
        result.largest=std::max(result.largest,largest); result.changed+=largest>=2;
        result.darker+=red+green+blue<=-6; result.brighter_red+=red>=2 && red>green;
    }
    return result;
}
}

int main(int argc,char** argv) {
    SDL_Window* window=nullptr;
    try {
        if (argc<2 || argc>3 || (std::string_view(argv[1])!="vulkan" && std::string_view(argv[1])!="dx12") || (argc==3 && std::string_view(argv[2])!="rr"))
            throw std::invalid_argument("usage: native_rt_smoke vulkan|dx12 [rr]");
        const bool vulkan=std::string_view(argv[1])=="vulkan",rr=argc==3;
        CHECK(SDL_Init(SDL_INIT_VIDEO));
        window=SDL_CreateWindow("Native RT parity",1280,720,SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE|(vulkan ? SDL_WINDOW_VULKAN : 0)); CHECK(window);
        uc::GameRenderer renderer(vulkan ? uc::RendererBackend::vulkan : uc::RendererBackend::directx12);
        renderer.configure_upscaling(rr ? uc::UpscalerKind::dlss : uc::UpscalerKind::blit,uc::UpscaleMode::native,false,.5,false,2);
        renderer.initialize(window,true,std::filesystem::path(SDL_GetBasePath())/"data/shaders",.5,16.0/9,false);
        CHECK(renderer.active_upscaler()==(rr ? uc::UpscalerKind::dlss : uc::UpscalerKind::blit));
        uint64_t serial=0;
        const auto submit=[&](gx::FramePacket packet) {
            packet.serial=++serial; const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
            while (!renderer.render(packet,true,serial)) { CHECK(std::chrono::steady_clock::now()<deadline); renderer.wait_for_pending(1000000); }
            bool done=false;
            while (!done) {
                for (auto retired : renderer.poll_completions()) done|=retired==serial;
                CHECK(std::chrono::steady_clock::now()<deadline); if (!done) renderer.wait_for_pending(1000000);
            }
        };
        const auto floor=mesh({{{-12,-1,-2},{12,-1,-2},{12,-1,-8},{-12,-1,-8}}},{0,1,0},{180,180,180,255});
        const auto wall=mesh({{{-12,-1,-6},{12,-1,-6},{12,3,-6},{-12,3,-6}}},{0,0,1},{255,0,0,255});
        const auto plate=mesh({{{-.5f,-1,-4},{.5f,-1,-4},{.5f,-.4f,-4},{-.5f,-.4f,-4}}},{0,0,1},{64,64,64,255});
        const auto roof=mesh({{{-1,.2f,-5},{1,.2f,-5},{1,.2f,-3},{-1,.2f,-3}}},{0,-1,0},{255,255,255,255});
        const auto surface=[&](std::shared_ptr<const gx::MeshBuffers> source,bool lit,bool glossy,bool skinned,float shift,float camera) {
            auto draw=quad({}, {}, {255,255,255,255}); draw.mesh=std::move(source); draw.vertices.clear(); draw.indices.clear();
            if (skinned) draw.state.edit().descriptors[0]=gx::Mode::direct;
            auto& transforms=draw.transforms.edit(); transforms.position_rows[0][3]=shift; transforms.position_rows[2][3]=camera;
            auto& uniforms=draw.uniforms.edit();
            if (lit) {
                uniforms.channels[0]=2|(1<<2)|(2<<7)|(3<<9); uniforms.channels[1]=2|(1<<9);
                uniforms.materials[0]={.8f,.6f,.4f,1}; uniforms.ambient[0]={.3f,.3f,.3f,1};
                const float specular=glossy ? 1.f : 0.f; uniforms.materials[1]={specular,specular,specular,1};
                uniforms.lights[0].color={80,80,80,255}; uniforms.lights[0].position={0,5,0,0};
                uniforms.lights[0].cosine={1,0,0,0}; uniforms.lights[0].distance={1,0,0,0};
            }
            return draw;
        };
        const auto frame=[&](uc::GraphicsEffects effects,bool lit=true,bool glossy=true,float shift=0,float camera=0,bool occluders=true) {
            effects.anisotropy=1; renderer.set_effects(effects);
            gx::FramePacket scene; scene.present=false; scene.clear_color={16,16,16,255};
            scene.draws.push_back(surface(floor,lit,glossy,false,0,camera)); scene.draws.push_back(surface(wall,false,false,false,0,camera));
            if (occluders) { scene.draws.push_back(surface(plate,false,false,true,shift,camera)); scene.draws.push_back(surface(roof,false,false,false,shift,camera)); }
            submit(std::move(scene));
            gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
            auto scene_copy=std::make_shared<gx::TexturePixels>(); scene_copy->gpu_only=true; scene_copy->levels={{640,360,{}}};
            boundary.texture_copy=gx::TextureCopy{{0,0,640,360},scene_copy}; submit(std::move(boundary));
            gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false; submit(std::move(empty));
            gx::FramePacket hud; hud.display_source={0,0,640,360};
            hud.draws.push_back(quad({{{.6f,-.9f,0},{.9f,-.9f,0},{.9f,-.6f,0},{.6f,-.6f,0}}},{0,0,1},{255,0,0,255},false));
            submit(std::move(hud)); auto pixels=renderer.capture(serial); hud_unchanged(pixels); CHECK(renderer.validation_errors()==0); return pixels;
        };
        const auto settle=[&](uc::GraphicsEffects effects,bool lit=true,bool glossy=true,bool occluders=true) {
            // Blit has an eight-frame jitter cycle; captures must share its phase.
            for (int i=0;i<23;++i) frame(effects,lit,glossy,0,0,occluders);
            return frame(effects,lit,glossy,0,0,occluders);
        };
        uc::GraphicsEffects off; off.anisotropy=1; frame(off); auto baseline=settle(off);
        renderer.set_bloom({false,100});
        const auto bloom_disabled=settle(off);
        if(!rr && bloom_disabled.rgba!=baseline.rgba) {
            uc::save_png(baseline,std::filesystem::path(SDL_GetBasePath())/"rt-default-before.png");
            uc::save_png(bloom_disabled,std::filesystem::path(SDL_GetBasePath())/"rt-default-after.png");
            CHECK(bloom_disabled.rgba==baseline.rgba);
        }
        baseline=bloom_disabled;
        const auto dump=[&](const char* name,const uc::RenderPixels& pixels) {
            uc::save_png(pixels,std::filesystem::path(SDL_GetBasePath())/(std::string("rt-")+argv[1]+"-"+name+(rr ? "-rr" : "")+".png"));
        };
        dump("off",baseline);
        const auto check_stats=[&] {
            const auto stats=renderer.rt_stats();
            std::fprintf(stderr,"%s: BLAS %u skinned %u static %u instances %u geometries %u hits %u records %u triangles %llu\n",argv[1],stats.blas,stats.skinned_blas,stats.static_instances,stats.instances,stats.geometries,stats.hit_geometries,stats.hit_records,(unsigned long long)stats.triangles);
            CHECK(stats.blas==1 && stats.skinned_blas==1 && stats.static_instances==3 && stats.instances==4);
            CHECK(stats.geometries==1 && stats.triangles==2 && stats.hit_geometries==4 && stats.hit_records==4);
        };
        if(!vulkan) {
            // Settings can change between non-presenting packets of one guest frame.
            // Keep eight presents so the later Vulkan/native captures share the blit jitter phase.
            uc::GraphicsEffects toggled; toggled.anisotropy=1; toggled.ray_tracing=true; toggled.rt_local_shadows=uc::EffectLevel::high; toggled.ray_reconstruction=rr;
            const auto hidden=[&](std::shared_ptr<const gx::MeshBuffers> source) {
                gx::FramePacket packet; packet.present=false; packet.clear_after_copy=false;
                packet.draws.push_back(surface(std::move(source),true,true,false,0,0)); submit(std::move(packet));
            };
            const auto finish=[&] {
                gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                auto copy=std::make_shared<gx::TexturePixels>(); copy->gpu_only=true; copy->levels={{640,360,{}}};
                boundary.texture_copy=gx::TextureCopy{{0,0,640,360},copy}; submit(std::move(boundary));
                gx::FramePacket hud; hud.display_source={0,0,640,360};
                hud.draws.push_back(quad({{{.6f,-.9f,0},{.9f,-.9f,0},{.9f,-.6f,0},{.6f,-.6f,0}}},{0,0,1},{255,0,0,255},false));
                submit(std::move(hud)); hud_unchanged(renderer.capture(serial)); CHECK(renderer.validation_errors()==0);
            };
            hidden(floor); renderer.set_effects(toggled); renderer.set_effects(off); hidden(plate); finish();
            CHECK(renderer.rt_stats().instances==0); // Cancelling before the boundary must discard the pending enable.
            int stale_aim_calls=0,current_aim_calls=0;
            renderer.set_aim_source([&] { ++stale_aim_calls; return std::array<float,2>{}; });
            hidden(floor); renderer.set_effects(toggled);
            renderer.set_aim_source([&] { ++current_aim_calls; return std::array<float,2>{}; });
            hidden(plate); finish();
            gx::FramePacket aim_packet; aim_packet.present=false; aim_packet.clear_after_copy=false;
            auto aim_probe=quad({{{-.1f,-.1f,0},{.1f,-.1f,0},{.1f,.1f,0},{-.1f,.1f,0}}},{0,0,1},{255,255,255,255},false);
            aim_probe.late_aim=true; aim_probe.aim_exact=true; aim_probe.blend.color_write=aim_probe.blend.alpha_write=false;
            aim_packet.draws.push_back(std::move(aim_probe)); submit(std::move(aim_packet));
            CHECK(current_aim_calls==1 && stale_aim_calls==0); // Applying pending effects must preserve the newest aim callback.
            renderer.set_aim_source({});
            frame(toggled); check_stats();
            hidden(floor); renderer.set_effects(off); hidden(wall); renderer.set_effects(toggled); hidden(plate); finish();
            frame(toggled); check_stats();
            for(int i=0;i<3;++i) frame(off);
            std::fprintf(stderr,"dx12 mid-frame RT cancel/enable/off/on: PASS\n");
        } else {
            // Match the native regression's frame count for deterministic RT sample seeds.
            for(int i=0;i<8;++i)frame(off);
        }
        for (const auto level : {uc::EffectLevel::low,uc::EffectLevel::high}) {
            for (const std::string_view signal : {"shadow","ao","reflection","lighting","path"}) {
                uc::GraphicsEffects effects; effects.ray_tracing=true; effects.ray_reconstruction=rr;
                if (signal=="shadow") effects.rt_local_shadows=level;
                if (signal=="ao") effects.rt_ambient_occlusion=level;
                if (signal=="reflection") effects.rt_reflections=level;
                if (signal=="lighting") effects.rt_lighting=level;
                if (signal=="path") { effects.path_tracing=true; effects.rt_quality=level; }
                const auto image=settle(effects); check_stats();
                const auto diff=floor_difference(baseline,image);
                std::fprintf(stderr,"%s %s: changed %zu darker %zu redder %zu largest %d\n",signal.data(),level==uc::EffectLevel::low ? "low" : "high",diff.changed,diff.darker,diff.brighter_red,diff.largest);
                CHECK(diff.changed>=20);
                if (signal=="shadow" || signal=="ao") CHECK(diff.darker>=20);
                if (signal=="reflection" || signal=="lighting") CHECK(diff.brighter_red>=5);
                const std::string name=std::string(signal)+(level==uc::EffectLevel::low ? "-low" : "-high"); dump(name.c_str(),image);
                if (!rr) {
                    if (signal!="path") { auto disabled=effects; disabled.ray_tracing=false; const auto negative=settle(disabled); CHECK(floor_difference(baseline,negative).largest<=1); }
                    if (signal=="reflection") { const auto matte_off=settle(off,true,false),matte_on=settle(effects,true,false); CHECK(floor_difference(matte_off,matte_on).largest<=1); }
                    if (signal=="ao") { const auto unlit_off=settle(off,false),unlit_on=settle(effects,false); CHECK(floor_difference(unlit_off,unlit_on).largest<=1); }
                }
            }
        }
        uc::GraphicsEffects moving; moving.path_tracing=true; moving.rt_quality=uc::EffectLevel::high; moving.ray_reconstruction=rr;
        for (int i=0;i<12;++i) frame(moving,true,true,float(i)*.05f,float(i)*.02f);
        frame(moving,true,true,-2,1); // Camera/instance jump exercises rejection of stale history.
        frame(off); const auto reset=settle(moving); dump("reset",reset);
        const auto rebuild_wait=[] { std::this_thread::sleep_for(uc::kRebuildInterval+std::chrono::milliseconds(10)); };
        renderer.set_render_scale(1); rebuild_wait(); frame(moving); frame(moving); CHECK(renderer.validation_errors()==0);
        renderer.set_render_scale(.5); rebuild_wait(); settle(moving);
        renderer.resize(0,0); gx::FramePacket suspended; suspended.serial=serial+1; CHECK(!renderer.render(suspended));
        renderer.resize(1280,720); rebuild_wait(); const auto resumed=settle(moving); dump("resumed",resumed); CHECK(renderer.validation_errors()==0);
        if(!vulkan) {
            // Resize resets scene-boundary detection, so its warm-up presents do not advance
            // the blit jitter sequence. Exercise it after all phase-matched image comparisons.
            gx::FramePacket before; before.present=false; before.clear_after_copy=false;
            before.draws.push_back(surface(floor,true,true,false,0,0)); submit(std::move(before));
            CHECK(SDL_SetWindowSize(window,1152,648) && SDL_SyncWindow(window)); renderer.resize(1152,648);
            gx::FramePacket after; after.present=false; after.clear_after_copy=false;
            after.draws.push_back(surface(plate,true,true,true,0,0)); submit(std::move(after));
            gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
            auto copy=std::make_shared<gx::TexturePixels>(); copy->gpu_only=true; copy->levels={{640,360,{}}};
            boundary.texture_copy=gx::TextureCopy{{0,0,640,360},copy}; submit(std::move(boundary));
            gx::FramePacket hud; hud.display_source={0,0,640,360};
            hud.draws.push_back(quad({{{.6f,-.9f,0},{.9f,-.9f,0},{.9f,-.6f,0},{.6f,-.6f,0}}},{0,0,1},{255,0,0,255},false));
            submit(std::move(hud)); hud_unchanged(renderer.capture(serial)); CHECK(renderer.validation_errors()==0);
            CHECK(SDL_SetWindowSize(window,1280,720) && SDL_SyncWindow(window)); renderer.resize(1280,720);
            frame(moving); frame(moving); check_stats();
            std::fprintf(stderr,"dx12 mid-frame RT resize: PASS\n");
        }
        renderer.shutdown(); SDL_DestroyWindow(window); SDL_Quit();
        std::printf("native_rt_smoke %s%s: PASS\n",argv[1],rr ? " RR" : ""); return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"native_rt_smoke: %s\n",error.what()); if (window) SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }
}
