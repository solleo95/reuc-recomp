#include "native_renderer.h"
#include "upscaler_probe.h"
#include "upscale_math.h"
#include <intrin.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
gx::DrawPacket quad(float x0,float y0,float x1,float y1,std::array<uint8_t,4> color,bool perspective) {
    gx::DrawPacket draw; draw.primitive=0x90; draw.callsite=perspective ? 0x1234 : 0x5678; draw.indices={0,1,2,0,2,3};
    for(const auto& p:std::array<std::array<float,2>,4>{{{x0,y0},{x1,y0},{x1,y1},{x0,y1}}}) {
        gx::Vertex vertex; vertex.position={p[0],p[1],perspective ? -1.f : .5f}; vertex.color[0]=color; vertex.normal={0,0,1,0,0,0,0,0,0}; draw.vertices.push_back(vertex);
    }
    draw.state.edit().viewport=std::array<float,6>{0,0,640,360,0,1};
    draw.uniforms.edit().pixel[1]=perspective ? 1u|16u|(3u<<1) : 0;
    if(perspective) draw.transforms.edit().projection={{{1,0,0,0},{0,1,0,0},{0,0,-4.f/3,-2.f/3},{0,0,-1,0}}};
    return draw;
}
std::array<uint8_t,4> pixel(const uc::RenderPixels& image,uint32_t x,uint32_t y) {
    const size_t offset=(size_t(y)*image.width+x)*4;
    return {image.rgba.at(offset),image.rgba.at(offset+1),image.rgba.at(offset+2),image.rgba.at(offset+3)};
}
void check_hud(const uc::RenderPixels& image) {
    require(image.rgba.size()==size_t(image.width)*image.height*4,"capture size mismatch");
    // The green HUD is drawn after reconstruction and must stay exact with FG on/off and after resize.
    for(uint32_t y=image.height/2-4;y<image.height/2+4;++y)
        for(uint32_t x=image.width*7/10-4;x<image.width*7/10+4;++x)
            require(pixel(image,x,y)==std::array<uint8_t,4>{0,255,0,255},"frame generation changed the HUD");
    const auto scene=pixel(image,image.width*3/10,image.height/2);
    require(scene[0]>=250 && scene[1]<=2 && scene[2]<=2,"moving perspective scene did not survive reconstruction");
    const auto depth=pixel(image,image.width*3/16,image.height/2);
    require(depth[0]>=250 && depth[2]<=2,"HUD depth test lost reconstructed scene depth");
}
}
int main(int argc,char** argv) {
    SDL_Window* window=nullptr;
    std::unique_ptr<uc::NativeRenderer> renderer;
    try {
        if(argc!=2 || (std::string_view(argv[1])!="dlss" && std::string_view(argv[1])!="fsr"))
            throw std::invalid_argument("usage: native_frame_generation_smoke dlss|fsr");
        const auto kind=std::string_view(argv[1])=="dlss" ? uc::UpscalerKind::dlss : uc::UpscalerKind::fsr;
        require(SDL_Init(SDL_INIT_VIDEO),SDL_GetError());
        const auto directory=std::filesystem::path(SDL_GetBasePath());
        const auto support=uc::probe_native_upscalers(directory,uc::RendererBackend::directx12);
        std::printf("DX12 FG probe: DLSS=%d FG=%d max=%u (%s), FSR=%d FG=%d (%s)\n",support.dlss,support.dlss_g,support.dlss_g_max_multiplier,support.dlss_g_reason.c_str(),support.fsr,support.fsr_fg,support.fsr_fg_reason.c_str());
        require(kind==uc::UpscalerKind::dlss ? support.dlss && support.dlss_g : support.fsr && support.fsr_fg,"requested native frame generation SDK is unavailable");
        window=SDL_CreateWindow("DX12 native frame generation fixture",1280,720,SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE);
        require(window!=nullptr,SDL_GetError());
        require(SDL_SetWindowFocusable(window,false),SDL_GetError());
        require(SDL_ShowWindow(window),SDL_GetError());
        SDL_PumpEvents();
        renderer=uc::make_d3d12_renderer();
        const bool warm_sr=kind==uc::UpscalerKind::dlss;
        require(renderer->set_upscaling(kind,uc::UpscaleMode::quality,false,.5,!warm_sr,2),"startup FG configuration rejected");
        renderer->set_bloom({false,100}); renderer->set_hdr({false,1000,200,200});
        renderer->initialize(window,true,directory/"data/shaders",warm_sr ? 1.5 : 1,16./9,false);
        require(renderer->active_upscaler()==kind && renderer->frame_generation_active()==!warm_sr,"native upscaler startup mode mismatch");
        uint64_t serial=0;
        const auto submit=[&](gx::FramePacket packet,bool display) {
            const auto submission=++serial; packet.serial=submission+10000;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
            bool submitted=false,retired=false;
            while(!retired) {
                if(!submitted) submitted=renderer->render(packet,display,submission);
                for(const auto completed:renderer->poll_completions()) retired|=completed==submission;
                require(std::chrono::steady_clock::now()<deadline,"native FG GPU retirement timed out");
                if(!retired) renderer->wait_for_pending(1000000);
            }
            SDL_PumpEvents();
        };
        const auto frame=[&](float shift,bool scene=true,bool display=true,bool black=false) {
            if(scene) {
                gx::FramePacket packet; packet.present=false; packet.clear_after_copy=false; packet.black_output=black;
                auto moving=quad(-1,-1,.1f,1,{255,0,0,255},true); moving.transforms.edit().position_rows[0][3]=shift;
                packet.draws.push_back(std::move(moving)); submit(std::move(packet),false);
                gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false; boundary.black_output=black;
                auto copy=std::make_shared<gx::TexturePixels>(); copy->gpu_only=true; copy->levels={{640,360,{}}};
                boundary.texture_copy=gx::TextureCopy{{0,0,640,360},copy}; submit(std::move(boundary),false);
                require(renderer->copy_ready(copy),"scene boundary copy was not retired");
            }
            gx::FramePacket hud; hud.display_source={0,0,640,360}; hud.black_output=black;
            hud.draws.push_back(quad(.3f,-.5f,.6f,.5f,{0,255,0,255},false));
            if(scene) {
                auto hidden=quad(-.7f,-.3f,-.5f,.3f,{0,0,255,255},false); hidden.uniforms.edit().pixel[1]=1u|(3u<<1);
                for(auto& vertex:hidden.vertices) vertex.position[2]=.9f;
                hud.draws.push_back(std::move(hidden));
            }
            submit(std::move(hud),display);
        };
        const auto animate=[&](int frames) { for(int i=0;i<frames;++i) frame(.06f*std::sin(float(i)*.13f)); };
        const auto captured=[&] { auto image=renderer->capture(serial); require(image.serial==serial,"capture ignored effective submission serial"); return image; };
        if(warm_sr) {
            // Enabling DLSS-G after SR recreates the device and Streamline plugins in the same process.
            for(const auto mode:{uc::UpscaleMode::quality,uc::UpscaleMode::performance,uc::UpscaleMode::native}) {
                require(renderer->set_upscaling(kind,mode,false,.5,false,2),"SR warm-up configuration rejected");
                animate(16); check_hud(captured());
            }
            require(renderer->stats().validation_errors==0,"SR warm-up produced graphics validation errors");
            renderer->shutdown(); renderer.reset(); renderer=uc::make_d3d12_renderer(); serial=0;
            require(renderer->set_upscaling(kind,uc::UpscaleMode::quality,false,.5,true,2),"SR to FG restart rejected");
            renderer->set_bloom({false,100});renderer->set_hdr({false,1000,200,200});
            renderer->initialize(window,true,directory/"data/shaders",1,16./9,false);
            require(renderer->frame_generation_active(),"FG failed after SR renderer restart");
        }
        animate(96); check_hud(captured());
        auto stats=renderer->stats();
        std::printf("DX12 %s FG initial: rendered=%llu generated=%llu validation=%u\n",argv[1],static_cast<unsigned long long>(stats.presentations),static_cast<unsigned long long>(stats.generated_presentations),stats.validation_errors);
        require(stats.generated_presentations>0,"SDK reported zero generated presentations");
        require(stats.validation_errors==0,"native FG produced graphics validation errors");
        // Known translation verifies FG preserved temporal input vectors and the clean internal scene.
        frame(0); frame(.025f);
        const auto motion=renderer->capture_motion(); const auto vector=motion.at(motion.width/4,motion.height*3/8);
        require(std::abs(vector[0]+motion.width*.0125f)<.15f && std::abs(vector[1])<.05f,"native FG changed motion vectors");
        const auto scene=renderer->capture_scene(); const auto extent=uc::internal_extent({1280,960},uc::UpscaleMode::quality);
        require(scene.width==extent.width && scene.height==extent.height,"FG changed reconstruction resolution");
        const auto clean=pixel(scene,scene.width*7/10,scene.height*3/8);
        require(clean!=std::array<uint8_t,4>{0,255,0,255},"HUD leaked into scene capture");
        check_hud(captured());
        const auto assert_no_generated=[&](uint64_t before,const char* message) { renderer->capture(serial,true); require(renderer->stats().generated_presentations==before,message); };
        require(renderer->set_upscaling(kind,uc::UpscaleMode::quality,false,.5,false,2),"disabling FG failed");
        frame(0); frame(0); check_hud(captured());
        require(!renderer->frame_generation_active(),"FG stayed enabled after setting was disabled");
        const auto disabled=renderer->stats().generated_presentations; animate(8); assert_no_generated(disabled,"disabled FG generated stale frames"); check_hud(captured());
        const uint32_t changed_multiplier=kind==uc::UpscalerKind::dlss ? std::min(3u,support.dlss_g_max_multiplier) : 2;
        require(renderer->set_upscaling(kind,uc::UpscaleMode::performance,true,.8,true,changed_multiplier),"re-enabling FG with new SR settings failed");
        animate(64); check_hud(captured()); require(renderer->frame_generation_active(),"FG did not reactivate");
        require(renderer->stats().generated_presentations>disabled,"SDK did not resume frame generation");
        require(SDL_SetWindowSize(window,1152,648) && SDL_SyncWindow(window),SDL_GetError()); renderer->resize(1152,648);
        animate(16); const auto resized=captured(); require(resized.width==1152 && resized.height==648,"FG resize lost output extent"); check_hud(resized);
        renderer->set_hdr({false,1200,220,180}); renderer->hdr_state_changed();
        require(SDL_SetWindowSize(window,1280,720) && SDL_SyncWindow(window),SDL_GetError()); renderer->resize(1280,720);
        animate(16); check_hud(captured()); require(!renderer->hdr_active(),"HDR-off setting changed presentation encoding");
        // Settle the previous scene before asserting that menu-only frames produce no extras.
        frame(0,false); frame(0,false); captured();
        const auto menu_count=renderer->stats().generated_presentations;
        for(int i=0;i<8;++i) frame(0,false);
        assert_no_generated(menu_count,"menu-only frames reused stale scene tags");
        for(int i=0;i<8;++i) { gx::FramePacket empty; empty.display_source={0,0,640,360}; submit(std::move(empty),true); }
        assert_no_generated(menu_count,"draw-free frames reused stale scene tags");
        frame(0,true,true,true); captured(); const auto black_count=renderer->stats().generated_presentations;
        for(int i=0;i<8;++i) frame(.01f*i,true,true,true);
        const auto black=captured();
        for(size_t i=0;i<black.rgba.size();i+=4) require(black.rgba[i]==0 && black.rgba[i+1]==0 && black.rgba[i+2]==0,"black output was not black");
        assert_no_generated(black_count,"black frames reused stale scene tags");
        const auto hidden_stats=renderer->stats();
        for(int i=0;i<8;++i) frame(.01f*i,true,false);
        assert_no_generated(hidden_stats.generated_presentations,"skipped presents generated frames");
        require(renderer->stats().presentations==hidden_stats.presentations,"display=false submitted a present");
        animate(64); const auto final=captured(); check_hud(final);
        stats=renderer->stats(); require(stats.generated_presentations>hidden_stats.generated_presentations,"FG did not recover after menu/black/skipped frames");
        require(stats.validation_errors==0,"native FG lifecycle produced graphics validation errors");
        uc::save_png(final,directory/(std::string("frame-generation-dx12-")+argv[1]+".png"));
        std::printf("DX12 %s FG passed: rendered=%llu generated=%llu validation=%u, HUD/depth/motion/settings/resize/menu/black verified\n",argv[1],static_cast<unsigned long long>(stats.presentations),static_cast<unsigned long long>(stats.generated_presentations),stats.validation_errors);
        renderer->shutdown(); renderer.reset(); SDL_DestroyWindow(window); SDL_Quit(); return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"native frame generation smoke: %s\n",error.what());
        if(renderer) { try { renderer->shutdown(); } catch(const std::exception& cleanup) { std::fprintf(stderr,"native FG cleanup: %s\n",cleanup.what()); } renderer.reset(); }
        if(window) SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }
}
