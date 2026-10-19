// GPU checks of the graphics effects (docs/superpowers/specs/2026-10-19-graphics-enhancements-design.md): one section per effect, each run by its own
// flag so a failing effect names itself. --validation turns the Vulkan validation layer on and fails on any error.
#include "check.h"
#include "vulkan_renderer.h"
#include <SDL3/SDL_vulkan.h>
#include <chrono>
#include <cmath>
#include <set>
#include <string>
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
// A quad from (x0,y0) to (x1,y1) in NDC as two triangles of one draw (the same fixture shape as renderer_upscale_smoke).
static gx::DrawPacket quad(uint32_t callsite, float x0, float y0, float x1, float y1, std::array<uint8_t,4> color, bool perspective) {
    gx::DrawPacket d; d.callsite=callsite; d.primitive=0x90;
    const float z=perspective ? -1.f : 0.5f;
    const std::array<std::array<float,2>,4> corners{{{x0,y0},{x1,y0},{x1,y1},{x0,y1}}};
    for(const auto& c:corners) { gx::Vertex v; v.position={c[0],c[1],z}; v.color[0]=color; d.vertices.push_back(v); }
    d.indices={0,1,2,0,2,3};
    auto& t=d.transforms.edit();
    if(perspective) t.projection={{{1,0,0,0},{0,1,0,0},{0,0,-4.f/3,-2.f/3},{0,0,-1,0}}};
    else t.projection={{{1,0,0,0},{0,1,0,0},{0,0,0,0.5f},{0,0,0,1}}};
    d.state.edit().viewport=std::array<float,6>{0,0,640,360,0,1};
    d.uniforms.edit().pixel[1]=perspective ? 31u : 0u;
    return d;
}
// Texture draw: one TEV stage passing texture 0 (the stage words of the scratch-workspace restore draw).
static void textured(gx::DrawPacket& draw, const std::shared_ptr<const gx::TexturePixels>& texture, uint8_t wrap) {
    draw.texture=texture; draw.texture_wrap[0]=wrap; draw.material.stages=1; draw.material.swaps={0xE4,0xE4,0xE4,0xE4};
    draw.material.stage[0]={0x8FFF8,0x8FFC0,0x3C0,0,0};
}
// A 64x64 checker of 4x4 squares (255 / 32) with a full box-filtered mip chain down to 1x1.
static std::shared_ptr<const gx::TexturePixels> checker() {
    auto pixels=std::make_shared<gx::TexturePixels>();
    std::vector<uint8_t> level(64*64*4);
    for(uint32_t y=0;y<64;++y) for(uint32_t x=0;x<64;++x) { const uint8_t v=((x/16)+(y/16))%2 ? 32 : 255; for(int c=0;c<3;++c) level[(y*64+x)*4+c]=v; level[(y*64+x)*4+3]=255; }
    for(uint32_t size=64;size>=1;size/=2) {
        pixels->levels.push_back({size,size,level});
        if(size==1) break;
        std::vector<uint8_t> next(size/2*size/2*4);
        for(uint32_t y=0;y<size/2;++y) for(uint32_t x=0;x<size/2;++x) for(int c=0;c<4;++c) {
            const auto at=[&](uint32_t px,uint32_t py) { return int(level[(py*size+px)*4+c]); };
            next[(y*(size/2)+x)*4+c]=uint8_t((at(2*x,2*y)+at(2*x+1,2*y)+at(2*x,2*y+1)+at(2*x+1,2*y+1)+2)/4);
        }
        level=std::move(next);
    }
    return pixels;
}
int main(int argc, char** argv) {
    bool validation=false;
    std::set<std::string> sections;
    for (int i=1;i<argc;++i) { const std::string arg=argv[i]; if(arg=="--validation") validation=true; else if(arg.rfind("--",0)==0) sections.insert(arg.substr(2)); else { std::fprintf(stderr,"usage: renderer_effects_smoke [--validation] [--aniso]\n"); return 2; } }
    const auto wanted=[&](const char* name) { return sections.empty() || sections.contains(name); };
    SDL_Window* window=nullptr;
    try {
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
        window=SDL_CreateWindow("Umbrella Chronicles — effects smoke", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        if (!window) throw std::runtime_error(SDL_GetError());
        ShowWindow(static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr)),SW_SHOWNOACTIVATE);
        CHECK(SDL_SyncWindow(window));
        uc::VulkanRenderer renderer;
        renderer.configure_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2);
        renderer.initialize(window, validation, executable_dir() / "data" / "shaders", 1.0);
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
        if (wanted("aniso")) {
            // A checker floor receding to the horizon, and a flat textured square minified evenly. Textures sample trilinearly (wrap 213: repeat, linear, linear mips).
            const auto texture=checker();
            auto frame=[&](uint32_t level) {
                uc::GraphicsEffects effects; effects.anisotropy=level; renderer.set_effects(effects);
                gx::FramePacket packet; packet.present=true; packet.display_source={0,0,640,360};
                auto floor=quad(0x1111,-1,-1,1,1,{255,255,255,255},true);
                const float far_z=-12, near_z=-1, y=-0.5f, half=4;
                const std::array<std::array<float,3>,4> corner{{{-half,y,near_z},{half,y,near_z},{half,y,far_z},{-half,y,far_z}}};
                for(size_t i=0;i<4;++i) { floor.vertices[i].position=corner[i]; floor.vertices[i].texcoord[0]={corner[i][0],-corner[i][2]}; }
                auto& rows=floor.transforms.edit().projection;
                rows[2]={0,0,-1.0256f,-0.5128f};   // GX depth: near 0.5 -> 0, far 20 -> 1
                textured(floor,texture,213);
                packet.draws.push_back(std::move(floor));
                auto flat=quad(0x2222,0.55f,-0.9f,0.95f,-0.3f,{255,255,255,255},false);   // screen-space, minified ~ 2.6x evenly
                for(size_t i=0;i<4;++i) flat.vertices[i].texcoord[0]={i==1||i==2 ? 1.f : 0.f, i>=2 ? 1.f : 0.f};
                textured(flat,texture,213);
                packet.draws.push_back(std::move(flat));
                return renderer.capture(submit(std::move(packet)));
            };
            const auto sharp=frame(16);
            const auto blur=frame(1);
            CHECK(sharp.width==1280 && sharp.height==720 && blur.width==sharp.width);
            if(const char* dump=std::getenv("UC_SMOKE_DUMP")) { uc::save_png(sharp,std::filesystem::path(dump)/"aniso-16.png"); uc::save_png(blur,std::filesystem::path(dump)/"aniso-1.png"); }   // debugging aid
            const auto deviation=[&](const uc::RenderPixels& pixels,uint32_t x0,uint32_t x1,uint32_t y0,uint32_t y1) {
                double sum=0,sum2=0; size_t n=0;
                for(uint32_t y=y0;y<y1;++y) for(uint32_t x=x0;x<x1;++x) { const double v=pixels.rgba[(size_t(y)*pixels.width+x)*4]; sum+=v; sum2+=v*v; ++n; }
                const double mean=sum/n; return std::sqrt(std::max(0.0,sum2/n-mean*mean));
            };
            const double sharp_band=deviation(sharp,300,980,318,338), blur_band=deviation(blur,300,980,318,338);   // near the horizon, where the footprint is longest
            std::fprintf(stderr,"aniso: floor band deviation 16x %.1f, 1x %.1f\n",sharp_band,blur_band);
            CHECK(sharp_band>blur_band*1.25);   // the grazing floor keeps its checker with anisotropic filtering
            // The evenly minified HUD square has an isotropic footprint: both levels agree.
            int largest=0;
            for(uint32_t y=45;y<240;++y) for(uint32_t x=1000;x<1240;++x)
                for(int c=0;c<3;++c) largest=std::max(largest,std::abs(int(sharp.rgba[(size_t(y)*sharp.width+x)*4+c])-int(blur.rgba[(size_t(y)*blur.width+x)*4+c])));
            std::fprintf(stderr,"aniso: flat square largest difference %d\n",largest);
            CHECK(largest<=2);
            CHECK(renderer.effective_anisotropy()==1);   // the last frame asked for 1
        }
        CHECK(renderer.validation_errors()==0);
        renderer.shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        std::fprintf(stderr,"renderer_effects_smoke: ok\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"renderer_effects_smoke failed: %s\n",error.what());
        if (window) SDL_DestroyWindow(window);
        return 1;
    }
}
