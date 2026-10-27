#include "check.h"
#include "game_renderer.h"
#include "upscale_math.h"
#include "upscaler_probe.h"
#include "rebuild_gate.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <thread>

int main(int argc,char** argv) {
    SDL_Window* window=nullptr;
    try {
        if(argc!=3)throw std::invalid_argument("usage: native_upscale_smoke vulkan|dx11|dx12 dlss|fsr|blit");
        const std::string_view api=argv[1],kind_name=argv[2];
        if(api!="vulkan" && api!="dx11" && api!="dx12")throw std::invalid_argument("invalid renderer");
        if(kind_name!="dlss" && kind_name!="fsr" && kind_name!="blit")throw std::invalid_argument("invalid upscaler");
        const auto backend=api=="vulkan" ? uc::RendererBackend::vulkan : api=="dx11" ? uc::RendererBackend::directx11 : uc::RendererBackend::directx12;
        const auto kind=kind_name=="dlss" ? uc::UpscalerKind::dlss : kind_name=="fsr" ? uc::UpscalerKind::fsr : uc::UpscalerKind::blit;
        CHECK(SDL_Init(SDL_INIT_VIDEO));window=SDL_CreateWindow("DirectX temporal upscaling parity",1280,720,SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE|(api=="vulkan" ? SDL_WINDOW_VULKAN : 0));CHECK(window);
        const auto directory=std::filesystem::path(SDL_GetBasePath());
        if(backend!=uc::RendererBackend::vulkan && kind!=uc::UpscalerKind::blit) {
            const auto support=uc::probe_native_upscalers(directory,backend);
            std::printf("probe %s DLSS=%d (%s) FSR=%d (%s) effects=%d\n",argv[1],support.dlss,support.dlss_reason.c_str(),support.fsr,support.fsr_reason.c_str(),support.effects);
            CHECK(support.effects && (kind==uc::UpscalerKind::dlss ? support.dlss : support.fsr));
        }
        for(const auto mode:{uc::UpscaleMode::native,uc::UpscaleMode::quality,uc::UpscaleMode::performance}) {
            uc::GameRenderer renderer(backend);renderer.configure_upscaling(kind,mode,false,.5,false,2);
            renderer.initialize(window,true,directory/"data/shaders",1,16./9,false);CHECK(renderer.active_upscaler()==kind);
            renderer.set_bloom({false,100});
            uint64_t serial=0;
            const auto submit=[&](gx::FramePacket packet) {
                packet.serial=++serial;const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);bool sent=false,done=false;
                while(!done) {
                    if(!sent)sent=renderer.render(packet);
                    for(auto value:renderer.poll_completions())done|=value==serial;
                    CHECK(std::chrono::steady_clock::now()<end);if(!done)renderer.wait_for_pending(1000000);
                }
            };
            const auto quad=[](float x0,float y0,float x1,float y1,std::array<uint8_t,4> color,bool perspective) {
                gx::DrawPacket d;d.primitive=0x90;d.callsite=perspective ? 0x1234 : 0x5678;d.indices={0,1,2,0,2,3};
                for(auto p:std::array<std::array<float,2>,4>{{{x0,y0},{x1,y0},{x1,y1},{x0,y1}}}) {gx::Vertex v;v.position={p[0],p[1],perspective ? -1.f : .5f};v.color[0]=color;d.vertices.push_back(v);}
                d.state.edit().viewport=std::array<float,6>{0,0,640,360,0,1};d.uniforms.edit().pixel[1]=perspective ? 1u|16u|(3u<<1) : 0;
                if(perspective)d.transforms.edit().projection={{{1,0,0,0},{0,1,0,0},{0,0,-4.f/3,-2.f/3},{0,0,-1,0}}};
                return d;
            };
            const auto frame=[&](float shift=0) {
                gx::FramePacket scene;scene.present=false;scene.draws.push_back(quad(-1,-1,.1f,1,{255,0,0,255},true));scene.draws[0].transforms.edit().position_rows[0][3]=shift;submit(std::move(scene));
                gx::FramePacket boundary;boundary.present=false;boundary.clear_after_copy=false;auto copy=std::make_shared<gx::TexturePixels>();copy->gpu_only=true;copy->levels={{640,360,{}}};boundary.texture_copy=gx::TextureCopy{{0,0,640,360},copy};submit(std::move(boundary));
                gx::FramePacket hud;hud.display_source={0,0,640,360};hud.draws.push_back(quad(.3f,-.5f,.6f,.5f,{0,255,0,255},false));
                auto hidden=quad(-.7f,-.3f,-.5f,.3f,{0,0,255,255},false);hidden.uniforms.edit().pixel[1]=1u|(3u<<1);for(auto& v:hidden.vertices)v.position[2]=.9f;hud.draws.push_back(std::move(hidden));submit(std::move(hud));
            };
            for(int i=0;i<32;++i)frame();
            const auto image=renderer.capture(serial),scene=renderer.capture_scene();const auto expected=uc::internal_extent({1280,960},mode);
            std::printf("%s %s mode%d scene%ux%u expected%ux%u\n",argv[1],argv[2],int(mode),scene.width,scene.height,expected.width,expected.height);
            CHECK(scene.width==expected.width && scene.height==expected.height);CHECK(image.width==1280 && image.height==720);
            const auto pixel=[&](uint32_t x,uint32_t y,size_t c) {return image.rgba[(size_t(y)*image.width+x)*4+c];};
            CHECK(pixel(400,360,0)>=250 && pixel(400,360,1)<=2);CHECK(pixel(900,360,1)==255 && pixel(900,360,0)==0);
            CHECK(pixel(240,360,0)>=250 && pixel(240,360,2)<=2); // HUD depth test sees upscaled scene depth.
            uc::save_png(image,directory/(std::string("upscale-")+argv[1]+"-"+argv[2]+"-"+std::to_string(int(mode))+".png"));
            frame(.025f);const auto motion=renderer.capture_motion();const auto vector=motion.at(motion.width/4,motion.height*3/8);
            CHECK(std::abs(vector[0]+motion.width*.0125f)<.1f && std::abs(vector[1])<.05f);
            CHECK(renderer.set_upscaling(kind,uc::UpscaleMode::quality,true,.8,false,2));for(int i=0;i<8;++i)frame();
            CHECK(SDL_SetWindowSize(window,1000,600) && SDL_SyncWindow(window));renderer.resize(1000,600);std::this_thread::sleep_for(uc::kRebuildInterval);frame();CHECK(renderer.capture(serial).width==1000);
            CHECK(SDL_SetWindowSize(window,1280,720) && SDL_SyncWindow(window));renderer.resize(1280,720);std::this_thread::sleep_for(uc::kRebuildInterval);frame();CHECK(renderer.capture(serial).width==1280);
            CHECK(renderer.validation_errors()==0);renderer.shutdown();
        }
        SDL_DestroyWindow(window);SDL_Quit();std::puts("native SDK upscale, motion, HUD depth, settings and resize passed");return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"native upscale smoke: %s\n",e.what());if(window)SDL_DestroyWindow(window);SDL_Quit();return 1;}
}
