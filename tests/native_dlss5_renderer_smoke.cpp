// Real-DLL GameRenderer integration counterpart of renderer_dlss5_smoke. No backend injection.
#include "game_renderer.h"
#include "ngx_dlss5.h"
#include <intrin.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace {
void require(bool ok,const std::string& reason) { if(!ok) throw std::runtime_error(reason); }
gx::DrawPacket quad(uint32_t callsite,float x0,float y0,float x1,float y1,bool perspective,const std::shared_ptr<const gx::TexturePixels>& texture) {
    gx::DrawPacket draw; draw.callsite=callsite; draw.primitive=0x90; draw.indices={0,1,2,0,2,3}; draw.texture=texture;
    const std::array<std::array<float,2>,4> points{{{x0,y0},{x1,y0},{x1,y1},{x0,y1}}},uv{{{0,1},{1,1},{1,0},{0,0}}};
    for(size_t i=0;i<points.size();++i) {
        gx::Vertex vertex; vertex.position={points[i][0],points[i][1],perspective ? -1.f : .5f}; vertex.texcoord[0]=uv[i]; vertex.color[0]={255,255,255,255};
        vertex.normal={0,0,1,0,0,0,0,0,0}; draw.vertices.push_back(vertex);
    }
    draw.state.edit().viewport=std::array<float,6>{0,0,640,360,0,1}; draw.uniforms.edit().pixel[1]=perspective ? 1u|16u|(3u<<1) : 0;
    if(perspective) draw.transforms.edit().projection={{{1,0,0,0},{0,1,0,0},{0,0,-4.f/3,-2.f/3},{0,0,-1,0}}};
    return draw;
}
std::shared_ptr<const gx::TexturePixels> pattern(bool hud) {
    auto texture=std::make_shared<gx::TexturePixels>(); texture->levels={{128,128,std::vector<uint8_t>(128*128*4)}};
    auto& rgba=texture->levels[0].rgba;
    for(uint32_t y=0;y<128;++y) for(uint32_t x=0;x<128;++x) {
        const size_t at=(size_t(y)*128+x)*4;
        rgba[at]=uint8_t((hud ? 70 : 32)+x*(hud ? 100 : 185)/128);
        rgba[at+1]=uint8_t((hud ? 45 : 20)+y*(hud ? 130 : 200)/128);
        rgba[at+2]=uint8_t((hud ? 90 : 48)+((x/11+y/13)%2)*(hud ? 90 : 80)); rgba[at+3]=255;
    }
    return texture;
}
size_t changed(const uc::RenderPixels& a,const uc::RenderPixels& b,std::array<uint32_t,4> region,int tolerance) {
    require(a.width==b.width && a.height==b.height,"capture dimensions changed unexpectedly"); size_t count=0;
    for(uint32_t y=region[1];y<region[1]+region[3];++y) for(uint32_t x=region[0];x<region[0]+region[2];++x) {
        const size_t at=(size_t(y)*a.width+x)*4; bool different=false;
        for(size_t c=0;c<3;++c) different|=std::abs(int(a.rgba.at(at+c))-int(b.rgba.at(at+c)))>tolerance;
        count+=different;
    }
    return count;
}
void black(const uc::RenderPixels& image) {
    for(size_t at=0;at<image.rgba.size();at+=4) require(image.rgba[at]==0 && image.rgba[at+1]==0 && image.rgba[at+2]==0,"black frame exposed processed scene pixels");
}
}
int main(int argc,char** argv) {
    SDL_Window* window=nullptr;
    std::unique_ptr<uc::GameRenderer> renderer;
    try {
        require(argc<=2,"usage: native_dlss5_renderer_smoke [runtime-and-data-directory]");
        require(SDL_Init(SDL_INIT_VIDEO),SDL_GetError());
        const auto directory=argc==2 ? std::filesystem::absolute(argv[1]) : std::filesystem::path(SDL_GetBasePath());
        require(std::filesystem::exists(directory/uc::kDlss5Runtime),"real DLSS 5 runtime is missing in test directory");
        window=SDL_CreateWindow("DX12 DLSS 5 renderer hooks",1280,720,SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE); require(window!=nullptr,SDL_GetError());
        renderer=std::make_unique<uc::GameRenderer>(uc::RendererBackend::directx12);
        renderer->allow_dlss5(true); renderer->configure_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::native,false,.5,false,2);
        renderer->set_bloom({false,100}); renderer->set_hdr({false,1000,200,200});
        renderer->initialize(window,true,directory/"data/shaders",1,16./9,false);
        renderer->set_bloom({false,100});
        require(renderer->dlss5_status().empty(),"DLSS 5 initialization: "+renderer->dlss5_status());
        const auto scene_texture=pattern(false),hud_texture=pattern(true);
        uint64_t serial=0;
        const auto submit=[&](gx::FramePacket packet) {
            const auto effective=++serial; packet.serial=effective+10000;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60); bool sent=false,done=false;
            while(!done) {
                if(!sent) sent=renderer->render(packet,true,effective);
                for(auto retired:renderer->poll_completions()) done|=retired==effective;
                require(std::chrono::steady_clock::now()<deadline,"DLSS 5 renderer GPU retirement timed out");
                if(!done) renderer->wait_for_pending(1000000);
            }
        };
        const auto frame=[&](bool blank=false,float camera=0,bool scene=true) {
            if(scene) {
                gx::FramePacket world; world.present=false; world.clear_after_copy=false; world.black_output=blank;
                auto geometry=quad(0x1234,-1,-1,1,1,true,scene_texture); geometry.transforms.edit().projection[0][2]=camera;
                world.draws.push_back(std::move(geometry)); submit(std::move(world));
                gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false; boundary.black_output=blank;
                auto copy=std::make_shared<gx::TexturePixels>(); copy->gpu_only=true; copy->levels={{640,360,{}}}; boundary.texture_copy=gx::TextureCopy{{0,0,640,360},copy}; submit(std::move(boundary));
                require(renderer->copy_ready(copy),"DLSS 5 scene boundary copy did not complete");
                gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false; empty.black_output=blank; submit(std::move(empty));
            }
            gx::FramePacket hud; hud.display_source={0,0,640,360}; hud.clear_after_copy=false; hud.black_output=blank;
            hud.draws.push_back(quad(0x5678,.3f,-.5f,.8f,.5f,false,hud_texture)); submit(std::move(hud));
        };
        const auto capture=[&](bool efb=false) { auto image=renderer->capture(serial,efb); require(image.serial==serial,"DLSS 5 capture used guest serial instead of submission serial"); return image; };
        const auto settle=[&](int count=4) { for(int n=0;n<count;++n) frame(); return capture(); };
        const auto active=[&] { require(renderer->dlss5_active(),"DLSS 5 hook did not run: "+renderer->dlss5_status()); require(renderer->dlss5_status().empty(),renderer->dlss5_status()); };
        uc::Dlss5Settings settings;
        renderer->set_dlss5(settings); const auto baseline=settle(); require(!renderer->dlss5_active(),"Off hook reports active");
        require(baseline.width==1280 && baseline.height==720,"unexpected baseline extent");
        const std::array<uint32_t,4> scene_region{40,40,700,640},hud_region{850,200,280,300};
        for(const auto hook:{uc::Dlss5Hook::render,uc::Dlss5Hook::upscaled,uc::Dlss5Hook::present}) {
            settings=uc::Dlss5Settings{}; settings.hook=hook; renderer->set_dlss5(settings); const auto result=settle(6); active();
            const auto scene_changes=changed(result,baseline,scene_region,2),hud_changes=changed(result,baseline,hud_region,0);
            std::printf("DX12 DLSS 5 hook %u: scene changed=%zu, HUD changed=%zu\n",unsigned(hook),scene_changes,hud_changes);
            require(scene_changes>size_t(scene_region[2])*scene_region[3]/100,"hook produced no meaningful scene change");
            if(hook==uc::Dlss5Hook::present) require(changed(result,baseline,hud_region,2)>100,"present hook did not process the HUD region");
            else require(hud_changes==0,"scene hook processed HUD drawn after the scene boundary");
            uc::save_png(result,directory/("dlss5-renderer-dx12-hook-"+std::to_string(unsigned(hook))+".png"));
            renderer->set_dlss5({}); const auto off=settle(); require(!renderer->dlss5_active(),"disabling hook retained active status");
            require(off.rgba==baseline.rgba,"disabled DLSS 5 changed original frame");
        }
        // Black-output frames still contain a fully rendered EFB. Comparing that EFB makes block_hudless meaningful.
        frame(true); const auto black_reference=capture(); const auto unprocessed_efb=capture(true); black(black_reference);
        settings.hook=uc::Dlss5Hook::render; settings.block_hudless=true; renderer->set_dlss5(settings);
        frame(true); frame(true); frame(true); black(capture()); require(capture(true).rgba==unprocessed_efb.rgba,"block_hudless processed the black frame's EFB");
        require(renderer->dlss5_status().empty(),renderer->dlss5_status());
        // ran()/active reports the latest record() result; skipped black/menu frames intentionally preserve that established API contract.
        const auto recovered=settle(6); active(); require(changed(recovered,baseline,scene_region,2)>1000,"DLSS 5 failed to recover after black reset");
        frame(false,.7f); active(); const auto camera_reset=settle(6); active();
        require(changed(camera_reset,baseline,scene_region,2)>1000,"camera reset stopped DLSS 5 evaluation");
        settings.hook=uc::Dlss5Hook::upscaled; settings.passes=2; settings.resolution_scale=.75; settings.upsample_filter=uc::Dlss5UpFilter::bicubic;
        renderer->set_dlss5(settings); const auto multiple=settle(5); active(); require(changed(multiple,baseline,scene_region,2)>1000,"multipass hook produced no processed scene");
        require(changed(multiple,baseline,hud_region,0)==0,"multipass scene hook changed later HUD");
        require(SDL_SetWindowSize(window,1152,648) && SDL_SyncWindow(window),SDL_GetError()); renderer->resize(1152,648);
        SDL_Delay(200); // Let window resize notification/debounce settle before measuring the new output.
        const auto resized=settle(5); active(); require(resized.width==1152 && resized.height==648,"DLSS 5 resize lost output extent");
        renderer->set_render_scale(.75); settle(5); active();
        renderer->set_render_scale(1); require(SDL_SetWindowSize(window,1280,720) && SDL_SyncWindow(window),SDL_GetError()); renderer->resize(1280,720);
        SDL_Delay(200);
        settle(5); active();
        renderer->set_dlss5({}); require(settle().rgba==baseline.rgba,"resize/reset left stale processed EFB when disabled"); require(!renderer->dlss5_active(),"Off hook did not reset status after resize");
        require(renderer->validation_errors()==0,"DLSS 5 hook integration produced validation errors"); renderer->shutdown();
        // Startup checkbox disabled: saved hook remains configured, feature stays off and the frame stays original.
        renderer=std::make_unique<uc::GameRenderer>(uc::RendererBackend::directx12); renderer->allow_dlss5(false); renderer->set_bloom({false,100});
        settings=uc::Dlss5Settings{}; settings.hook=uc::Dlss5Hook::present; renderer->set_dlss5(settings);
        renderer->initialize(window,true,directory/"data/shaders",1,16./9,false);
        renderer->set_bloom({false,100}); renderer->set_hdr({false,1000,200,200});
        require(settle().rgba==baseline.rgba,"startup DLSS 5 disable changed output"); require(!renderer->dlss5_active(),"startup-disabled DLSS 5 ran");
        require(!renderer->dlss5_status().empty(),"startup-disabled DLSS 5 omitted status reason"); require(renderer->validation_errors()==0,"disabled DLSS 5 validation failed");
        renderer->shutdown(); renderer.reset(); SDL_DestroyWindow(window); SDL_Quit();
        std::puts("native DLSS 5 GameRenderer hooks/off/black/reset/resize/multipass passed"); return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"native DLSS 5 renderer smoke: %s\n",error.what());
        if(renderer) { try { renderer->shutdown(); } catch(const std::exception& cleanup) { std::fprintf(stderr,"DLSS 5 renderer cleanup: %s\n",cleanup.what()); } renderer.reset(); }
        if(window) SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }
}
