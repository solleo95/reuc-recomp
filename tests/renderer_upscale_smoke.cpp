// GPU check of the upscaling frame structure with the test-only blit upscaler: the 3D scene renders at the mode's internal resolution,
// the HUD after the scene boundary renders at output resolution, motion vectors follow a moved draw, targets follow the render scale.
#include "check.h"
#include "vulkan_renderer.h"
#include <SDL3/SDL_vulkan.h>
#include <chrono>
#include <cmath>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
using namespace std::chrono_literals;

static std::filesystem::path executable_dir() {
    std::vector<wchar_t> path(32768);
    DWORD size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!size || size >= path.size()) throw std::runtime_error("executable path unavailable");
    return std::filesystem::path(std::wstring(path.data(), size)).parent_path();
}
// A quad from (x0,y0) to (x1,y1) in NDC as two triangles of one draw.
static gx::DrawPacket quad(uint32_t callsite, float x0, float y0, float x1, float y1, std::array<uint8_t,4> color, bool perspective) {
    gx::DrawPacket d; d.callsite=callsite; d.primitive=0x90;
    const float z=perspective ? -1.f : 0.5f;
    const std::array<std::array<float,2>,4> corners{{{x0,y0},{x1,y0},{x1,y1},{x0,y1}}};
    for(const auto& c:corners) { gx::Vertex v; v.position={c[0],c[1],z}; v.color[0]=color; d.vertices.push_back(v); }
    d.indices={0,1,2,0,2,3};
    auto& t=d.transforms.edit();
    if(perspective) t.projection={{{1,0,0,0},{0,1,0,0},{0,0,-4.f/3,-2.f/3},{0,0,-1,0}}};   // GX style, near 0.5 far 2: w = -z = 1, depth 2/3 (invertible, as the game's)
    else t.projection={{{1,0,0,0},{0,1,0,0},{0,0,0,0.5f},{0,0,0,1}}};
    d.state.edit().viewport=std::array<float,6>{0,0,640,360,0,1};
    d.uniforms.edit().pixel[1]=perspective ? 31u : 0u;   // compare always + update for the scene, no depth for the HUD
    return d;
}
static gx::DrawPacket restored_quarter(const std::shared_ptr<const gx::TexturePixels>& saved) {
    auto restore=quad(0x80044340,-1,-1,0,0,{255,255,255,255},false);
    restore.texture=saved; restore.material.stages=1; restore.material.swaps={0xE4,0xE4,0xE4,0xE4};
    restore.material.stage[0]={0x8FFF8,0x8FFC0,0x3C0,0,0};
    restore.vertices[0].texcoord[0]={0,0}; restore.vertices[1].texcoord[0]={1,0};
    restore.vertices[2].texcoord[0]={1,1}; restore.vertices[3].texcoord[0]={0,1};
    return restore;
}
int main(int argc, char** argv) {
    bool validation=false, dlss=false, fsr=false, framegen=false;   // --framegen additionally exercises both optional presentation backends
    for (int i=1;i<argc;++i) { const std::string arg=argv[i]; validation|=arg=="--validation"; dlss|=arg=="--dlss"; fsr|=arg=="--fsr"; framegen|=arg=="--framegen"; }
    SDL_Window* window = nullptr;
    try {
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
        window = SDL_CreateWindow("Umbrella Chronicles — upscale smoke", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        if (!window) throw std::runtime_error(SDL_GetError());
        ShowWindow(static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr)),SW_SHOWNOACTIVATE);
        CHECK(SDL_SyncWindow(window));
        uc::VulkanRenderer renderer;
        renderer.configure_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::performance,false,0.5,false,2);
        renderer.initialize(window, validation, executable_dir() / "data" / "shaders", 1.0);
        CHECK(renderer.set_upscaling(uc::UpscalerKind::blit,uc::UpscaleMode::performance,false,0.5,false,2));
        uint64_t serial=0;
        auto submit=[&](gx::FramePacket packet) {
            packet.serial=++serial;
            auto end=std::chrono::steady_clock::now()+3s;
            bool submitted=false, completed=false;
            while(!completed && std::chrono::steady_clock::now()<end) {
                SDL_Event event; while(SDL_PollEvent(&event)) if(event.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) renderer.resize(uint32_t(event.window.data1),uint32_t(event.window.data2));
                if(!submitted) submitted=renderer.render(packet);
                if(submitted) renderer.wait_for_pending(1000000);
                for(auto value:renderer.poll_completions()) completed|=value==packet.serial;
                std::this_thread::sleep_for(1ms);
            }
            CHECK(submitted && completed);
            return packet.serial;
        };
        // One guest frame: scene packet, the game's scene copy (the boundary), HUD + present.
        auto frame=[&](float shift) {   // the scene quad moves by its position matrix, as GX objects do
            gx::FramePacket scene; scene.present=false;
            scene.draws.push_back(quad(0x1234,-1,-1,0.1f,1,{255,0,0,255},true));
            scene.draws.back().transforms.edit().position_rows[0][3]=shift;
            submit(std::move(scene));
            gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
            auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
            boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
            submit(std::move(boundary));
            gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
            submit(std::move(empty));   // an empty post-boundary packet preserves the reconstructed EFB
            gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
            hud.draws.push_back(quad(0x5678,0.3f,-0.5f,0.6f,0.5f,{0,255,0,255},false));
            return submit(std::move(hud));
        };
        frame(0);   // the first frame decides scene mode for the next one
        const auto shown=frame(0);
        CHECK((renderer.upscale_internal_extent()==uc::RenderExtent{640,480}));   // performance: half of the 1280x960 EFB
        const auto pixels=renderer.capture(shown);
        CHECK(pixels.width==1280 && pixels.height==720);
        auto at=[&](uint32_t x,uint32_t y) { return &pixels.rgba[(size_t(y)*pixels.width+x)*4]; };
        size_t blended_scene=0, blended_hud=0;
        for(uint32_t x=690;x<716;++x) { const auto red=at(x,360)[0]; blended_scene+=red>16 && red<255; }   // scene edge at x 704
        for(uint32_t x=820;x<846;++x) { const auto green=at(x,360)[1]; blended_hud+=green>32 && green<255; } // HUD edge at x 832
        CHECK(blended_scene>=1);   // reconstructed from 640x360
        CHECK(blended_hud==0);     // drawn at 1280x720
        CHECK(at(400,360)[0]==255 && at(700,100)[1]==0);
        // Motion: the scene quad moves right by 8 internal pixels (NDC 0.025): previous minus current = -8 px in x.
        frame(0.025f);
        const auto motion=renderer.capture_motion();
        CHECK(motion.width==640 && motion.height==480);
        const auto mv=motion.at(200,180);
        std::fprintf(stderr,"motion at (200,180): %g %g; at (600,180): %g %g\n",mv[0],mv[1],motion.at(600,180)[0],motion.at(600,180)[1]);
        CHECK(std::abs(mv[0]+8)<0.05f && std::abs(mv[1])<0.05f);
        const auto still=motion.at(600,180);   // background: no depth-writing draw, cleared to zero
        CHECK(still[0]==0 && still[1]==0);
        {   // a non-depth-writing pass preserves the underlying vectors while still drawing colour
            gx::FramePacket scene; scene.present=false;
            scene.draws.push_back(quad(0x1234,-1,-1,0.1f,1,{255,0,0,255},true));
            scene.draws.back().transforms.edit().position_rows[0][3]=0.05f;
            submit(std::move(scene));
            const auto before=renderer.capture_motion();
            gx::FramePacket overlay; overlay.present=false;
            overlay.draws.push_back(quad(0x3333,-1,-1,0,0,{0,255,0,255},false));
            overlay.draws.back().transforms.edit().position_rows[0][3]=0.2f;
            submit(std::move(overlay));
            const auto after=renderer.capture_motion();
            for(uint32_t y=0;y<after.height;y+=17) for(uint32_t x=0;x<after.width;x+=17) CHECK(before.at(x,y)==after.at(x,y));
            const auto colour=renderer.capture_scene();
            CHECK(colour.rgba[(size_t(100)*colour.width+100)*4+1]==255);
            gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
            auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
            boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
            submit(std::move(boundary));
            gx::FramePacket hud; hud.present=true; submit(std::move(hud));
        }
        {   // the present's clear reaches the scene target: the next frame's scene starts from the clear colour, not the last frame's pixels
            gx::FramePacket next; next.present=false;
            next.draws.push_back(quad(0x1234,-1,-1,0.1f,1,{255,0,0,255},true));
            next.draws.back().transforms.edit().position_rows[0][3]=0.05f;   // 16 internal pixels right of the start: x 8..15 was last frame's quad
            submit(std::move(next));
            const auto scene=renderer.capture_scene();
            CHECK(scene.width==640 && scene.rgba[(size_t(180)*scene.width+12)*4]==16 && scene.rgba[(size_t(180)*scene.width+20)*4]==255);
            gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
            auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
            boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
            submit(std::move(boundary));
            gx::FramePacket hud; hud.present=true; submit(std::move(hud));
        }
        // Render scale change recreates the scene targets (Review Focus 1).
        renderer.set_render_scale(1.5);
        std::this_thread::sleep_for(300ms);
        frame(0); frame(0);
        CHECK((renderer.upscale_internal_extent()==uc::RenderExtent{960,720}));
        CHECK(renderer.whole_frame_upscales()==0);   // every frame had the scene copy
        // Quality mode switch in place (in-game menu): the upscaler and scene targets are rebuilt at the new internal size.
        CHECK(renderer.set_upscaling(uc::UpscalerKind::blit,uc::UpscaleMode::quality,false,0.5,false,2));
        std::this_thread::sleep_for(300ms);   // the rebuild gate
        frame(0); frame(0);
        CHECK((renderer.upscale_internal_extent()==uc::RenderExtent{1280,960}));   // quality: 1/1.5 of the 1920x1440 EFB
        CHECK(renderer.active_upscaler()==uc::UpscalerKind::blit && !renderer.frame_generation_active());
        const auto device=renderer.device;
        for(int i=0;i<2;++i) {
            CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
            frame(0); frame(0);
            CHECK(renderer.active_upscaler()==uc::UpscalerKind::off && renderer.device==device);
            CHECK(renderer.set_upscaling(uc::UpscalerKind::blit,uc::UpscaleMode::quality,false,0.5,false,2));
            frame(0); frame(0);
            CHECK(renderer.active_upscaler()==uc::UpscalerKind::blit && renderer.device==device);
        }
        for(bool clear:{false,true}) {   // earlier snapshots still hold their original pixels if a later draw or clear changes the scene
            auto saved=std::make_shared<gx::TexturePixels>(); saved->gpu_only=true; saved->levels={{320,180,{}},{160,90,{}}};
            gx::FramePacket scene; scene.present=false; scene.clear_after_copy=clear;
            scene.draws.push_back(quad(0x1234,-1,-1,1,1,{255,0,0,255},true));
            scene.texture_copy=gx::TextureCopy{{0,0,320,180},saved};
            submit(std::move(scene));
            if(!clear) {
                gx::FramePacket later; later.present=false; later.clear_after_copy=false;
                later.draws.push_back(quad(0x1234,-1,-1,1,1,{0,255,0,255},true));
                submit(std::move(later));
            }
            gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
            auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
            boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
            submit(std::move(boundary));
            gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360}; hud.draws.push_back(restored_quarter(saved));
            const auto p=renderer.capture(submit(std::move(hud)));
            const auto i=(size_t(100)*p.width+100)*4;
            std::fprintf(stderr,"Earlier snapshot (clear %d): %u %u %u\n",int(clear),p.rgba[i],p.rgba[i+1],p.rgba[i+2]);
            CHECK(p.rgba[i]==255 && p.rgba[i+1]==0);
        }
        // Refresh is a second GPU use: retain its target independently of the original copy submission.
        std::weak_ptr<const gx::TexturePixels> refreshed;
        {
            auto saved=std::make_shared<gx::TexturePixels>(); saved->gpu_only=true; saved->levels={{320,180,{}}};
            gx::FramePacket scene; scene.present=false; scene.clear_after_copy=false;
            scene.draws.push_back(quad(0x1234,-1,-1,1,1,{255,0,0,255},true));
            scene.texture_copy=gx::TextureCopy{{0,0,320,180},saved};
            submit(std::move(scene));
            gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
            auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
            boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
            submit(std::move(boundary));
            refreshed=saved;
        }
        CHECK(refreshed.use_count()>=3);   // cache, original-copy slot, refresh slot; the guest has dropped its owner
        renderer.shutdown();
        CHECK(renderer.validation_errors()==0);
        // RCAS after the upscale (our pass for DLSS and the blit): a 2-pixel white stripe on gray, native mode (1:1 blit).
        CHECK(SDL_SetWindowSize(window,1280,720) && SDL_SyncWindow(window));
        std::this_thread::sleep_for(300ms);
        uc::VulkanRenderer sharpening_renderer;
        sharpening_renderer.configure_upscaling(uc::UpscalerKind::blit,uc::UpscaleMode::native,false,0.5,false,2);
        sharpening_renderer.initialize(window, validation, executable_dir() / "data" / "shaders", 1.0);
        uint64_t sharpening_serial=0;
        auto sharpened=[&](bool sharpening, double sharpness) {
            auto& r=sharpening_renderer;
            const auto device=r.device;
            CHECK(r.set_upscaling(uc::UpscalerKind::blit,uc::UpscaleMode::native,sharpening,sharpness,false,2));
            CHECK(r.device==device);
            auto send=[&](gx::FramePacket p) {
                p.serial=++sharpening_serial; p.clear_color={128,128,128,255};
                auto end=std::chrono::steady_clock::now()+3s; bool sent=false, done=false;
                while(!done && std::chrono::steady_clock::now()<end) {
                    SDL_PumpEvents();
                    if(!sent) sent=r.render(p);
                    if(sent) r.wait_for_pending(1000000);
                    for(auto value:r.poll_completions()) done|=value==p.serial;
                }
                CHECK(sent && done); return p.serial;
            };
            uint64_t shown=0;
            const int frames=sharpening_serial ? 8 : 3;   // subsequent settings finish at the same native jitter phase
            for(int f=0;f<frames;++f) {
                gx::FramePacket scene; scene.present=false;
                scene.draws.push_back(quad(0x1234,0,-1,2.f/640,1,{200,200,200,255},true));   // below white: RCAS keeps headroom, a 1.0 peak blocks it
                send(std::move(scene));
                gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                send(std::move(boundary));
                gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360}; hud.clear_after_copy=false;
                shown=send(std::move(hud));
            }
            const auto p=r.capture(shown);
            const auto at=[&](uint32_t x,uint32_t y) { return p.rgba[(size_t(y)*p.width+x)*4]; };
            CHECK(at(640,360)>=200 && at(641,360)>=200);
            const auto neighbour=[&](uint32_t begin) {
                uint8_t darkest=255;
                for(uint32_t x=begin;x<begin+5;++x) darkest=std::min(darkest,at(x,360));
                return darkest;
            };
            const std::array<uint8_t,3> result{neighbour(635),neighbour(642),at(100,360)};   // jitter can move the stripe by one pixel
            return result;
        };
        const auto off=sharpened(false,0.5), soft=sharpened(true,0.01), hard=sharpened(true,1.0);
        std::fprintf(stderr,"RCAS neighbours: off %u/%u soft %u/%u hard %u/%u flat %u %u %u\n",off[0],off[1],soft[0],soft[1],hard[0],hard[1],off[2],soft[2],hard[2]);
        CHECK(off[0]==128 && off[1]==128);
        CHECK(hard[0]<soft[0] && hard[1]<soft[1] && soft[0]<128 && soft[1]<128);   // the dark ring grows with the sharpness
        CHECK(std::abs(int(soft[2])-128)<=1 && std::abs(int(hard[2])-128)<=1);    // flat areas stay
        CHECK(sharpened(false,1.0)==off);   // disabling restores the original pixels on the same device
        sharpening_renderer.shutdown(); CHECK(sharpening_renderer.validation_errors()==0);
        // A still scene through DLSS converges to the exact edge coverage: the jitter sign, the vectors and the per-frame clear agree.
        // Measured on the RTX 4080: quality 19, DLAA 12 (a stale scene target gave 45, a flipped jitter 21-24 and an unstable DLAA).
        // --fsr runs the same scene through FSR 3.1 (its built-in sharpening off here).
        std::vector<uc::UpscalerKind> kinds; if (dlss) kinds.push_back(uc::UpscalerKind::dlss); if (fsr) kinds.push_back(uc::UpscalerKind::fsr);
        for (auto kind:kinds) for (auto [mode,limit]:{std::pair{uc::UpscaleMode::quality,25.0},std::pair{uc::UpscaleMode::native,16.0}}) {
            uc::VulkanRenderer r;
            r.configure_upscaling(kind,mode,false,0.5,false,2);
            r.initialize(window, validation, executable_dir() / "data" / "shaders", 1.0);
            CHECK(mode!=uc::UpscaleMode::quality || (r.upscale_internal_extent()==uc::RenderExtent{853,640}));   // 1280x960 EFB / 1.5
            uint64_t s=0;
            auto send=[&](gx::FramePacket p) {
                p.serial=++s; p.clear_color={128,128,128,255};
                auto end=std::chrono::steady_clock::now()+3s; bool sent=false, done=false;
                while(!done && std::chrono::steady_clock::now()<end) {
                    SDL_PumpEvents();
                    if(!sent) sent=r.render(p);
                    if(sent) r.wait_for_pending(1000000);
                    for(auto value:r.poll_completions()) done|=value==p.serial;
                }
                CHECK(sent && done); return p.serial;
            };
            std::vector<uc::RenderPixels> last;
            double restore_error=0; size_t restored_edges=0;
            for(int f=0;f<60;++f) {
                if(f==30) {
                    const auto device=r.device;
                    const auto start=std::chrono::steady_clock::now();
                    CHECK(r.set_upscaling(uc::UpscalerKind::off,mode,false,0.5,false,2));
                    gx::FramePacket native; native.present=true;
                    send(std::move(native));
                    CHECK(r.active_upscaler()==uc::UpscalerKind::off);
                    CHECK(r.set_upscaling(kind,mode,false,0.5,false,2));
                    CHECK(r.device==device && r.active_upscaler()==kind);
                    std::fprintf(stderr,"%s off/on on the same device: %.1f ms\n",kind==uc::UpscalerKind::dlss ? "DLSS" : "FSR",
                        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
                }
                gx::FramePacket scene; scene.present=false;
                auto tri=quad(0x1234,-0.6f,-0.6f,0.6f,0.6f,{230,40,40,255},true);
                tri.indices={0,1,2};   // one triangle: a diagonal edge
                scene.draws.push_back(std::move(tri));
                auto saved=std::make_shared<gx::TexturePixels>(); saved->gpu_only=true; saved->levels={{320,180,{}},{160,90,{}}};
                scene.texture_copy=gx::TextureCopy{{0,0,320,180},saved};   // game saves the scratch workspace before the upscale
                scene.clear_after_copy=false;
                send(std::move(scene));
                gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                const auto boundary_serial=send(std::move(boundary));
                const auto reconstructed=f>=58 ? r.capture(boundary_serial,true) : uc::RenderPixels{};
                gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};   // clears the EFB as the game's present does
                hud.draws.push_back(restored_quarter(saved));
                const auto shown=send(std::move(hud));
                if(f>=58) {
                    last.push_back(r.capture(shown));
                    for(uint32_t y=10;y<350;++y) for(uint32_t x=10;x<630;++x) {
                        const auto i=(size_t(y)*reconstructed.width+x)*4;
                        const auto reference=reconstructed.rgba[i];
                        if(reference>140 && reference<220) {
                            restore_error+=std::abs(int(reference)-int(last.back().rgba[i])); ++restored_edges;
                        }
                    }
                }
            }
            restore_error/=std::max<size_t>(restored_edges,1);
            std::fprintf(stderr,"%s restored quarter: edge error %.2f over %zu pixels\n",kind==uc::UpscalerKind::dlss ? "DLSS" : "FSR",restore_error,restored_edges);
            CHECK(restored_edges>100 && restore_error<8);   // restoration preserves reconstruction instead of putting raw jittered pixels back
            int worst=0; size_t blended=0;
            for(size_t i=0;i<last[0].rgba.size();i+=4) {
                worst=std::max(worst,std::abs(int(last[0].rgba[i])-int(last[1].rgba[i])));
                blended+=last[1].rgba[i]>140 && last[1].rgba[i]<220;   // between the gray and the red: anti-aliased edge
            }
            // Reference: exact coverage of the triangle per output pixel (8x8 samples), GX pixel centre offset -1/12 logical px (-1/6 output px).
            const auto& p=last[1];
            const auto inside=[&](double x,double y) {   // pixel space of the 1280x720 output
                const double nx=(x+1.0/6)/1280*2-1, ny=(y+1.0/6)/720*2-1;
                return nx<=0.6 && ny>=-0.6 && ny<=nx;   // the triangle (-0.6,-0.6) (0.6,-0.6) (0.6,0.6), y down
            };
            double error=0; size_t edge=0;
            for(uint32_t y=0;y<p.height;++y) for(uint32_t x=0;x<p.width;++x) {
                int covered=0;
                for(int sy=0;sy<8;++sy) for(int sx=0;sx<8;++sx) covered+=inside(x+(sx+0.5)/8,y+(sy+0.5)/8);
                if(covered==0 || covered==64) continue;
                const double expected=128+(230-128)*covered/64.0;
                error+=std::abs(p.rgba[(size_t(y)*p.width+x)*4]-expected); ++edge;
            }
            error/=std::max<size_t>(edge,1);
            std::fprintf(stderr,"%s still scene (mode %d): max change between the last frames %d, edge pixels blended %zu, edge error vs exact coverage %.2f over %zu px\n",kind==uc::UpscalerKind::dlss ? "DLSS" : "FSR",int(mode),worst,blended,error,edge);
            CHECK(worst<=12);      // the edge settles
            CHECK(blended>=200);   // the diagonal is reconstructed, not stair-stepped
            CHECK(error<=limit);   // and where the geometry is
            const auto device=r.device;
            const auto check_frame=[&] {
                gx::FramePacket p; p.present=true; p.display_source={0,0,640,360};
                p.draws.push_back(quad(0x1234,-1,-1,1,1,{230,40,40,255},true));
                const auto pixels=r.capture(send(std::move(p)));
                const auto i=(size_t(pixels.height/2)*pixels.width+pixels.width/2)*4;
                CHECK(pixels.rgba[i]>180 && pixels.rgba[i+1]<80);
            };
            if(framegen) for(bool generated:{true,false,true,false}) {
                CHECK(r.set_upscaling(kind,mode,false,0.5,generated,2));
                check_frame(); check_frame();
                CHECK(r.device==device && r.frame_generation_active()==generated);
            }
            if(dlss && fsr) {
                const auto other=kind==uc::UpscalerKind::dlss ? uc::UpscalerKind::fsr : uc::UpscalerKind::dlss;
                CHECK(r.set_upscaling(other,mode,false,0.5,false,2));
                check_frame(); check_frame();
                CHECK(r.device==device && r.active_upscaler()==other);
                if(framegen) {
                    CHECK(r.set_upscaling(other,mode,false,0.5,true,2));
                    check_frame(); check_frame();
                    CHECK(r.device==device && r.frame_generation_active());
                }
                CHECK(r.set_upscaling(uc::UpscalerKind::off,mode,false,0.5,false,2));
                check_frame();
                CHECK(r.device==device && r.active_upscaler()==uc::UpscalerKind::off && !r.frame_generation_active());
            }
            r.shutdown(); CHECK(r.validation_errors()==0);
        }
        SDL_DestroyWindow(window); SDL_Quit();
        std::fprintf(stderr,"Upscale smoke passed: internal scene, sharp HUD, motion vectors, render-scale change\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"upscale smoke failed: %s\n",error.what());
        if(window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
}
