// GPU checks of the graphics effects (docs/superpowers/specs/2026-10-19-graphics-enhancements-design.md): one section per effect, each run by its own
// flag so a failing effect names itself. --validation turns the Vulkan validation layer on and fails on any error.
#include "check.h"
#include "gbuffer.h"
#include "rt_denoiser.h"
#include "rt_light_math.h"
#include "rt_shadow_math.h"
#include "vulkan_renderer.h"
#include <algorithm>
#include <SDL3/SDL_vulkan.h>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
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
            {   // The game's textures have ONE mip level and sampler mode 0x95 (linear, no mip filter): anisotropy has to work there too (87% of the scene's draws).
                auto single=std::make_shared<gx::TexturePixels>(*checker()); single->levels.resize(1);
                auto frame_single=[&](uint32_t level) {
                    uc::GraphicsEffects effects; effects.anisotropy=level; renderer.set_effects(effects);
                    gx::FramePacket packet; packet.present=true; packet.display_source={0,0,640,360};
                    auto floor=quad(0x1111,-1,-1,1,1,{255,255,255,255},true);
                    const float far_z=-12, near_z=-1, y=-0.5f, half=4;
                    const std::array<std::array<float,3>,4> corner{{{-half,y,near_z},{half,y,near_z},{half,y,far_z},{-half,y,far_z}}};
                    for(size_t i=0;i<4;++i) { floor.vertices[i].position=corner[i]; floor.vertices[i].texcoord[0]={corner[i][0],-corner[i][2]}; }
                    floor.transforms.edit().projection[2]={0,0,-1.0256f,-0.5128f};
                    textured(floor,single,0x95);
                    packet.draws.push_back(std::move(floor));
                    return renderer.capture(submit(std::move(packet)));
                };
                const auto sharp_single=frame_single(16), plain_single=frame_single(1);
                int largest=0;
                for(uint32_t y=318;y<340;++y) for(uint32_t x=300;x<980;++x) largest=std::max(largest,std::abs(int(sharp_single.rgba[(size_t(y)*sharp_single.width+x)*4])-int(plain_single.rgba[(size_t(y)*plain_single.width+x)*4])));
                std::fprintf(stderr,"aniso: single-level texture, largest difference near the horizon %d\n",largest);
                CHECK(largest>=16);
            }
        }
        if (wanted("smaa")) {
            // SMAA 1x as the anti-aliasing choice: scene at the EFB size, no jitter, diagonal edges gain intermediate values, flat areas stay exact.
            CHECK(renderer.set_upscaling(uc::UpscalerKind::smaa,uc::UpscaleMode::quality,false,0.5,false,2));
            auto frame=[&]() {   // scene packet, the game's scene copy (the boundary), HUD + present
                gx::FramePacket scene; scene.present=false;
                auto triangle=quad(0x1234,-1,-1,1,1,{255,255,255,255},true);   // half of the screen, white on the dark clear colour (a luma step above SMAA's 0.1 threshold): a long diagonal edge
                triangle.vertices.resize(3); triangle.vertices[0].position={-1,-1,-1}; triangle.vertices[1].position={1,-1,-1}; triangle.vertices[2].position={-1,1,-1};
                triangle.indices={0,1,2};
                scene.draws.push_back(std::move(triangle));
                submit(std::move(scene));
                gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                submit(std::move(boundary));
                gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                submit(std::move(empty));
                gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                hud.draws.push_back(quad(0x5678,0.3f,-0.5f,0.6f,0.5f,{0,255,0,255},false));
                return renderer.capture(submit(std::move(hud)));
            };
            frame();   // the first frame decides scene mode for the next one
            const auto first=frame(), second=frame();
            if(const char* dump=std::getenv("UC_SMOKE_DUMP")) uc::save_png(first,std::filesystem::path(dump)/"smaa.png");   // debugging aid
            CHECK((renderer.upscale_internal_extent()==uc::RenderExtent{1280,960}));   // ratio 1: the scene renders at the EFB size
            const auto channel=[&](const uc::RenderPixels& pixels,uint32_t x,uint32_t y) { return int(pixels.rgba[(size_t(y)*pixels.width+x)*4]); };
            size_t blended=0;
            for(uint32_t y=60;y<340;++y) for(uint32_t x=60;x<1220;++x) { const int red=channel(first,x,y); blended+=red>24 && red<248; }   // between the clear colour (16) and the triangle (255)
            std::fprintf(stderr,"smaa: %zu pixels with intermediate values\n",blended);
            CHECK(blended>=200);
            CHECK(channel(first,100,100)==255 && channel(first,1200,500)==16);   // far from the edge: exact triangle (upper left) and clear colour (lower right)
            CHECK(first.rgba==second.rgba);   // no jitter: repeated frames are identical
            CHECK(renderer.pass_ms(uc::GpuPass::smaa)>0);
        }
        if (wanted("gbuffer")) {
            // The normal G-buffer: depth-writing draws write normal, fog and flags; draws that write no depth leave the surface below alone.
            CHECK(renderer.set_upscaling(uc::UpscalerKind::blit,uc::UpscaleMode::native,false,0.5,false,2));
            renderer.force_gbuffer(true);
            auto frame=[&]() {
                gx::FramePacket scene; scene.present=false;
                auto lit=quad(0x1234,-1,-1,0.2f,1,{255,255,255,255},true);   // x from -1 to 0.2, facing the camera, lighting on
                for(auto& vertex:lit.vertices) vertex.normal={0,0,1,0,0,0,0,0,0};
                lit.uniforms.edit().channels[0]=1|2;
                scene.draws.push_back(std::move(lit));
                scene.draws.push_back(quad(0x2345,-0.5f,-1,0.5f,1,{0,255,0,255},false));   // x from -0.5 to 0.5, no depth write: a blended-style overlay
                submit(std::move(scene));
                gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                submit(std::move(boundary));
                gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                submit(std::move(empty));
                gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                hud.draws.push_back(quad(0x5678,0.6f,-0.5f,0.9f,0.5f,{255,255,255,255},false));
                submit(std::move(hud));
            };
            frame();   // the first frame decides scene mode for the next one
            frame();
            auto buffer=renderer.capture_gbuffer();
            CHECK(buffer.width==1280 && buffer.height==960);
            const auto texel=[&](const uc::VulkanRenderer::GBufferPixels& pixels,uint32_t x,uint32_t y) { const size_t i=(size_t(y)*pixels.width+x)*4; return uc::decode_gbuffer({pixels.texels[i],pixels.texels[i+1],pixels.texels[i+2],pixels.texels[i+3]}); };
            const auto surface=texel(buffer,100,360);   // lit quad only
            std::fprintf(stderr,"gbuffer: surface flags %u normal z %.3f; overlap flags %u; overlay-only flags %u; background flags %u\n",surface.flags,surface.normal[2],texel(buffer,500,360).flags,texel(buffer,900,360).flags,texel(buffer,1200,360).flags);
            CHECK(surface.flags==(uc::kGBufferHasNormal|uc::kGBufferLit) && surface.normal[2]>0.99f);
            CHECK(texel(buffer,500,360).flags==(uc::kGBufferHasNormal|uc::kGBufferLit));   // the overlay did not overwrite the surface
            CHECK(texel(buffer,900,360).flags==0 && texel(buffer,1200,360).flags==0);      // overlay-only and empty pixels have no surface
            renderer.set_render_scale(2.0);
            frame(); frame();
            buffer=renderer.capture_gbuffer();
            CHECK(buffer.width==2560 && buffer.height==1920);
            CHECK(texel(buffer,200,720).flags==(uc::kGBufferHasNormal|uc::kGBufferLit));
            renderer.force_gbuffer(false);
            renderer.set_render_scale(1.0);   // the sections after this one expect the 1280x960 EFB
        }
        if (wanted("passthrough")) {
            // The scene-target path without an upscaler (what the screen-space effects run on): the picture equals the direct path, the targets follow the switch.
            CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
            uc::GraphicsEffects effects; effects.anisotropy=1; renderer.set_effects(effects);
            auto frame=[&]() {
                gx::FramePacket scene; scene.present=false;
                auto triangle=quad(0x1234,-1,-1,1,1,{255,255,255,255},true);
                triangle.vertices.resize(3); triangle.vertices[0].position={-1,-1,-1}; triangle.vertices[1].position={1,-1,-1}; triangle.vertices[2].position={-1,1,-1};
                triangle.indices={0,1,2};
                scene.draws.push_back(std::move(triangle));
                scene.draws.push_back(quad(0x2345,-0.2f,-0.4f,0.3f,0.6f,{200,60,20,255},true));
                submit(std::move(scene));
                gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                submit(std::move(boundary));
                gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                submit(std::move(empty));
                gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                hud.draws.push_back(quad(0x5678,0.6f,-0.5f,0.9f,0.5f,{0,255,0,255},false));
                return renderer.capture(submit(std::move(hud)));
            };
            frame(); const auto direct=frame();   // the first frame decides scene mode for the next one
            CHECK((renderer.upscale_internal_extent()==uc::RenderExtent{0,0}));
            renderer.force_scene_targets(true);
            frame(); const auto through=frame();
            CHECK((renderer.upscale_internal_extent()==uc::RenderExtent{1280,960}));   // ratio 1: the EFB size
            CHECK(direct.rgba==through.rgba);
            renderer.force_scene_targets(false);
            frame(); const auto again=frame();
            CHECK((renderer.upscale_internal_extent()==uc::RenderExtent{0,0}) && again.rgba==direct.rgba);
            renderer.force_scene_targets(true); frame(); frame();
            const auto pools=renderer.descriptor_pool_creations(), textures=renderer.texture_creations();
            for(int i=0;i<10;++i) { renderer.force_scene_targets(i%2==1); frame(); frame(); }
            CHECK(renderer.descriptor_pool_creations()-pools<=10 && renderer.texture_creations()-textures<=10);   // switching rebuilds targets, not the per-frame resources
            renderer.force_scene_targets(false);
        }
        if (wanted("gtao")) {
            // Ambient occlusion on a floor meeting a wall at 90 degrees: the crease darkens, flat areas and unfogged-away pixels stay put, fog fades it, menus skip it.
            CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
            const auto projection=[](gx::DrawPacket& draw) { draw.transforms.edit().projection={{{1,0,0,0},{0,1,0,0},{0,0,-100.f/99,-100.f/99},{0,0,-1,0}}}; };   // near 1, far 100, 90 degrees
            const auto surface=[&](std::array<std::array<float,3>,4> corners,std::array<float,3> normal,bool fogged,bool cut_out=false) {
                auto draw=quad(0x1000,-1,-1,1,1,{180,180,180,255},true);
                for(size_t i=0;i<4;++i) { draw.vertices[i].position=corners[i]; draw.vertices[i].normal={normal[0],normal[1],normal[2],0,0,0,0,0,0}; }
                projection(draw);
                if(cut_out) draw.uniforms.edit().pixel[0]=10u|(4u<<16)|(7u<<19);   // alpha compare "greater than 10" (everything passes here): a cut-out draw like the game's grates and nets
                if(fogged) { auto& uniforms=draw.uniforms.edit(); uniforms.fog[0]=280480; uniforms.fog[3]=(2u<<21)|(1u<<20); uniforms.fog[4]=0x808080; }   // linear fog, orthographic form, fully fogged everywhere
                return draw;
            };
            float bend_sign=1.f;
            auto frame=[&](uc::EffectLevel level,bool fogged,bool menu=false,bool cut_out_wall=false,bool bent_normals=false) {
                uc::GraphicsEffects effects; effects.anisotropy=1; effects.ambient_occlusion=level; renderer.set_effects(effects);
                gx::FramePacket scene; scene.present=false;
                if(!menu) {
                    // bent_normals: the floor's vertex normals lean 40 degrees toward the camera, as the smooth normals of a low-poly rod do against its flat facets
                    scene.draws.push_back(surface({{{-12,-1,-2},{12,-1,-2},{12,-1,-8},{-12,-1,-8}}},bent_normals ? std::array<float,3>{0,0.77f,bend_sign*0.64f} : std::array<float,3>{0,1,0},fogged));   // the floor
                    scene.draws.push_back(surface({{{-12,-1,-8},{12,-1,-8},{12,6,-8},{-12,6,-8}}},{0,0,1},fogged,cut_out_wall));      // the wall
                    submit(std::move(scene));
                    gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                    auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                    boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                    submit(std::move(boundary));
                    gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                    submit(std::move(empty));
                }
                gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                hud.draws.push_back(quad(0x5678,0.6f,-0.9f,0.9f,-0.6f,{255,0,0,255},false));
                return renderer.capture(submit(std::move(hud)));
            };
            const auto red=[&](const uc::RenderPixels& pixels,uint32_t x,uint32_t y) { return int(pixels.rgba[(size_t(y)*pixels.width+x)*4]); };
            frame(uc::EffectLevel::off,false);   // the first frame decides scene mode for the next one
            const auto base=frame(uc::EffectLevel::off,false);
            if(const char* dump=std::getenv("UC_SMOKE_DUMP")) uc::save_png(base,std::filesystem::path(dump)/"gtao-base.png");   // debugging aid
            CHECK(red(base,640,190)==180 && red(base,640,600)==180);   // flat floor and wall are the uniform colour
            for(const auto level:{uc::EffectLevel::low,uc::EffectLevel::medium,uc::EffectLevel::high,uc::EffectLevel::ultra}) {
                frame(level,false);
                const auto shaded=frame(level,false);
                int darkest=0;
                for(uint32_t y=300;y<335;++y) darkest=std::max(darkest,red(base,640,y)-red(shaded,640,y));   // within ~5% of the screen height of the crease (row 315)
                std::fprintf(stderr,"gtao %s: crease darkening %d, far floor %d, far wall %d\n",std::string(uc::effect_level_name(level)).c_str(),darkest,red(base,640,190)-red(shaded,640,190),red(base,640,600)-red(shaded,640,600));
                if(std::getenv("UC_SMOKE_DUMP")) { std::fprintf(stderr,"  rows:"); for(uint32_t y=182;y<640;y+=24) std::fprintf(stderr," %u:%d",y,red(shaded,640,y)); std::fprintf(stderr,"\n"); }   // debugging aid
                CHECK(darkest>=10);
                CHECK(std::abs(red(base,640,190)-red(shaded,640,190))<=6 && std::abs(red(base,640,600)-red(shaded,640,600))<=6);   // far from the crease: at most the sampling noise of a few slices (the crease itself loses 30+)
            }
            {   // alpha cut-out draws (grates, nets) are neither shaded nor occluders: a hatched net used to leave a dark pattern on whatever lies behind it
                frame(uc::EffectLevel::off,false,false,true);
                const auto cut_base=frame(uc::EffectLevel::off,false,false,true);
                frame(uc::EffectLevel::high,false,false,true);
                const auto cut_shaded=frame(uc::EffectLevel::high,false,false,true);
                int change=0;
                for(uint32_t y=182;y<640;y+=3) change=std::max(change,std::abs(red(cut_base,640,y)-red(cut_shaded,640,y)));
                std::fprintf(stderr,"gtao cut-out wall: largest change anywhere on the centre column %d\n",change);
                CHECK(change<=24);   // an opaque wall darkens its crease by 50-60 levels (54 with the cut-out wall before the fix); what is left is the sampling noise of a few slices on a grazing floor
            }
            {   // occlusion follows the geometry, not the vertex normals: a flat floor with bent normals stays as bright as it is (and no AO result is ever black)
                int worst=0;
                for(const float sign:{1.f,-1.f}) {
                    bend_sign=sign;
                    frame(uc::EffectLevel::off,false,false,true,true);
                    const auto bent_base=frame(uc::EffectLevel::off,false,false,true,true);
                    for(const auto level:{uc::EffectLevel::low,uc::EffectLevel::high}) {
                        frame(level,false,false,true,true);
                        const auto bent_shaded=frame(level,false,false,true,true);
                        for(uint32_t y=190;y<310;y+=3) worst=std::max(worst,red(bent_base,640,y)-red(bent_shaded,640,y));   // the floor is the upper band of the picture here
                    }
                }
                bend_sign=1.f;
                if(std::getenv("UC_SMOKE_DUMP")) { const auto g=renderer.capture_gbuffer(); const size_t i=(size_t(250)*g.width+640)*4; const auto t=uc::decode_gbuffer({g.texels[i],g.texels[i+1],g.texels[i+2],g.texels[i+3]}); std::fprintf(stderr,"  floor normal in the G-buffer: %.2f %.2f %.2f flags %u\n",t.normal[0],t.normal[1],t.normal[2],t.flags); }
                std::fprintf(stderr,"gtao bent normals: largest darkening of the flat floor %d\n",worst);
                CHECK(worst<=12);   // was 177 with the vertex normals; what is left is the sampling noise of a grazing floor
            }
            const auto fog_base=frame(uc::EffectLevel::off,true);
            frame(uc::EffectLevel::high,true);
            const auto fog_shaded=frame(uc::EffectLevel::high,true);
            for(uint32_t y=300;y<335;++y) CHECK(std::abs(red(fog_base,640,y)-red(fog_shaded,640,y))<=1);   // fully fogged: no occlusion
            const auto runs=renderer.pass_runs(uc::GpuPass::occlusion);
            CHECK(runs>0);
            const auto menu=frame(uc::EffectLevel::high,false,true);   // no scene this frame
            CHECK(renderer.pass_runs(uc::GpuPass::occlusion)==runs && red(menu,100,100)==16);
            frame(uc::EffectLevel::medium,false);
            auto before_pools=renderer.descriptor_pool_creations(), before_textures=renderer.texture_creations();
            for(int i=0;i<20;++i) frame(uc::EffectLevel::medium,false);
            const auto steady_pools=renderer.descriptor_pool_creations()-before_pools, steady_textures=renderer.texture_creations()-before_textures;   // what a frame costs anyway (the fixture's fresh copy target)
            before_pools=renderer.descriptor_pool_creations(); before_textures=renderer.texture_creations();
            for(int i=0;i<20;++i) frame(i%2 ? uc::EffectLevel::low : uc::EffectLevel::high,false);   // inside the effects path a level change swaps AO images only: no new per-frame resources
            CHECK(renderer.descriptor_pool_creations()-before_pools<=steady_pools+2 && renderer.texture_creations()-before_textures<=steady_textures+2);
            for(int i=0;i<10;++i) frame(i%2 ? uc::EffectLevel::off : uc::EffectLevel::medium,false);   // entering and leaving the path rebuilds the scene targets and must stay clean
            frame(uc::EffectLevel::off,false);
            frame(uc::EffectLevel::off,false);
        }
        if (wanted("contact")) {
            // Contact shadows toward the dominant directional light: a box on a floor, lit from the upper left. The floor right at the box's base darkens,
            // the far floor does not, a scene with only a point light has no shadow caster, and the shadows add to the ambient occlusion.
            CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
            const auto projection=[](gx::DrawPacket& draw) { draw.transforms.edit().projection={{{1,0,0,0},{0,1,0,0},{0,0,-100.f/99,-100.f/99},{0,0,-1,0}}}; };
            const auto surface=[&](std::array<std::array<float,3>,4> corners,std::array<float,3> normal,bool directional) {
                auto draw=quad(0x1000,-1,-1,1,1,{180,180,180,255},true);
                for(size_t i=0;i<4;++i) { draw.vertices[i].position=corners[i]; draw.vertices[i].normal={normal[0],normal[1],normal[2],0,0,0,0,0,0}; }
                projection(draw);
                auto& uniforms=draw.uniforms.edit();
                uniforms.channels[0]=1|2|(1<<2)|(2<<7);   // vertex colour as material, lighting on, light 0, clamped diffuse, no attenuation
                uniforms.ambient[0]={90,90,90,255};
                uniforms.lights[0].color={255,255,255,255}; uniforms.lights[0].direction={0,0,1,0};
                uniforms.lights[0].position=directional ? std::array<float,4>{-1e6f,1e6f,1e6f,0} : std::array<float,4>{-0.5f,1.f,-3.f,0};
                uniforms.lights[0].distance={1,0,0,0}; uniforms.lights[0].cosine={1,0,0,0};
                return draw;
            };
            auto frame=[&](bool contact,uc::EffectLevel ao,bool directional) {
                uc::GraphicsEffects effects; effects.anisotropy=1; effects.contact_shadows=contact; effects.ambient_occlusion=ao; renderer.set_effects(effects);
                gx::FramePacket scene; scene.present=false;
                scene.draws.push_back(surface({{{-12,-1,-2},{12,-1,-2},{12,-1,-8},{-12,-1,-8}}},{0,1,0},directional));   // the floor
                scene.draws.push_back(surface({{{-0.5f,-0.4f,-5},{0.5f,-0.4f,-5},{0.5f,-0.4f,-4},{-0.5f,-0.4f,-4}}},{0,1,0},directional));   // the box's top
                scene.draws.push_back(surface({{{-0.5f,-1,-4},{0.5f,-1,-4},{0.5f,-0.4f,-4},{-0.5f,-0.4f,-4}}},{0,0,1},directional));   // its front
                scene.draws.push_back(surface({{{0.5f,-1,-4},{0.5f,-1,-5},{0.5f,-0.4f,-5},{0.5f,-0.4f,-4}}},{1,0,0},directional));   // its right side
                submit(std::move(scene));
                gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                submit(std::move(boundary));
                gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                submit(std::move(empty));
                gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                hud.draws.push_back(quad(0x5678,0.6f,-0.9f,0.9f,-0.6f,{255,0,0,255},false));
                return renderer.capture(submit(std::move(hud)));
            };
            const auto red=[&](const uc::RenderPixels& pixels,uint32_t x,uint32_t y) { return int(pixels.rgba[(size_t(y)*pixels.width+x)*4]); };
            frame(false,uc::EffectLevel::off,true);   // the first frame decides scene mode for the next one
            const auto off=frame(false,uc::EffectLevel::off,true);
            if(const char* dump=std::getenv("UC_SMOKE_DUMP")) uc::save_png(off,std::filesystem::path(dump)/"contact-off.png");   // debugging aid
            frame(true,uc::EffectLevel::off,true);
            const auto on=frame(true,uc::EffectLevel::off,true);
            if(const char* dump=std::getenv("UC_SMOKE_DUMP")) uc::save_png(on,std::filesystem::path(dump)/"contact-on.png");
            int shadow=0, far_floor=0;
            for(uint32_t y=262;y<300;++y) for(uint32_t x=700;x<760;++x) shadow=std::max(shadow,red(off,x,y)-red(on,x,y));   // the floor just right of the box's base
            for(uint32_t x=100;x<500;x+=20) far_floor=std::max(far_floor,std::abs(red(off,x,200)-red(on,x,200)));
            std::fprintf(stderr,"contact: base shadow %d, far floor %d, dominant light %s\n",shadow,far_floor,renderer.dominant_light() ? "found" : "none");
            CHECK(renderer.dominant_light().has_value() && renderer.dominant_light()->kind==gx::LightKind::directional);
            CHECK(shadow>=15 && far_floor<=1);
            frame(false,uc::EffectLevel::off,false); const auto point_off=frame(false,uc::EffectLevel::off,false);
            frame(true,uc::EffectLevel::off,false); const auto point_on=frame(true,uc::EffectLevel::off,false);
            CHECK(!renderer.dominant_light().has_value() && point_on.rgba==point_off.rgba);   // a point light is no shadow caster: nothing changes
            frame(false,uc::EffectLevel::medium,true); const auto ao_only=frame(false,uc::EffectLevel::medium,true);
            frame(true,uc::EffectLevel::medium,true); const auto both=frame(true,uc::EffectLevel::medium,true);
            int added=0; for(uint32_t y=262;y<300;++y) for(uint32_t x=700;x<760;++x) added=std::max(added,red(ao_only,x,y)-red(both,x,y));
            CHECK(added>=10);   // both effects darken: the contact shadows go on top of the occlusion
            frame(false,uc::EffectLevel::off,true); frame(false,uc::EffectLevel::off,true);
        }
        if (wanted("shadows")) {
            // Shadow maps: the contact-shadow fixture (a box on a floor under a directional light) as mesh draws, which are the shadow casters.
            CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
            const auto projection=[](gx::DrawPacket& draw) { draw.transforms.edit().projection={{{1,0,0,0},{0,1,0,0},{0,0,-100.f/99,-100.f/99},{0,0,-1,0}}}; };
            const auto mesh_surface=[&](std::array<std::array<float,3>,4> corners,std::array<float,3> normal,bool directional) {
                auto draw=quad(0x1000,-1,-1,1,1,{180,180,180,255},true);
                for(size_t i=0;i<4;++i) { draw.vertices[i].position=corners[i]; draw.vertices[i].normal={normal[0],normal[1],normal[2],0,0,0,0,0,0}; }
                projection(draw);
                auto& uniforms=draw.uniforms.edit();
                uniforms.channels[0]=1|2|(1<<2)|(2<<7);
                uniforms.ambient[0]={90,90,90,255};
                uniforms.lights[0].color={255,255,255,255}; uniforms.lights[0].direction={0,0,1,0};
                uniforms.lights[0].position=directional ? std::array<float,4>{-1e6f,1e6f,1e6f,0} : std::array<float,4>{-0.5f,1.f,-3.f,0};
                uniforms.lights[0].distance={1,0,0,0}; uniforms.lights[0].cosine={1,0,0,0};
                draw.mesh=std::make_shared<const gx::MeshBuffers>(gx::MeshBuffers{draw.vertices,draw.indices});   // meshes cast shadows; inline vertices do not
                draw.vertices.clear(); draw.indices.clear();
                return draw;
            };
            auto frame=[&](uc::EffectLevel level,bool directional,bool menu=false) {
                uc::GraphicsEffects effects; effects.anisotropy=1; effects.shadows=level; renderer.set_effects(effects);
                gx::FramePacket scene; scene.present=false;
                if(!menu) {
                    scene.draws.push_back(mesh_surface({{{-12,-1,-2},{12,-1,-2},{12,-1,-8},{-12,-1,-8}}},{0,1,0},directional));
                    scene.draws.push_back(mesh_surface({{{-0.5f,-0.4f,-5},{0.5f,-0.4f,-5},{0.5f,-0.4f,-4},{-0.5f,-0.4f,-4}}},{0,1,0},directional));
                    scene.draws.push_back(mesh_surface({{{-0.5f,-1,-4},{0.5f,-1,-4},{0.5f,-0.4f,-4},{-0.5f,-0.4f,-4}}},{0,0,1},directional));
                    scene.draws.push_back(mesh_surface({{{0.5f,-1,-4},{0.5f,-1,-5},{0.5f,-0.4f,-5},{0.5f,-0.4f,-4}}},{1,0,0},directional));
                    submit(std::move(scene));
                    gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                    auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                    boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                    submit(std::move(boundary));
                    gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                    submit(std::move(empty));
                }
                gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                hud.draws.push_back(quad(0x5678,0.6f,-0.9f,0.9f,-0.6f,{255,0,0,255},false));
                return renderer.capture(submit(std::move(hud)));
            };
            const auto red=[&](const uc::RenderPixels& pixels,uint32_t x,uint32_t y) { return int(pixels.rgba[(size_t(y)*pixels.width+x)*4]); };
            renderer.force_lights(true);   // the light of the previous frame is what the map is drawn with: collect it without the shadows being on yet
            frame(uc::EffectLevel::off,true); frame(uc::EffectLevel::off,true);
            const auto off=frame(uc::EffectLevel::off,true);
            frame(uc::EffectLevel::medium,true); frame(uc::EffectLevel::medium,true);
            const auto on=frame(uc::EffectLevel::medium,true);
            if(const char* dump=std::getenv("UC_SMOKE_DUMP")) { uc::save_png(off,std::filesystem::path(dump)/"shadow-off.png"); uc::save_png(on,std::filesystem::path(dump)/"shadow-on.png"); }   // debugging aid
            int shadow=0, far_floor=0;
            for(uint32_t y=262;y<340;++y) for(uint32_t x=640;x<900;++x) shadow=std::max(shadow,red(off,x,y)-red(on,x,y));   // the floor right of and behind the box
            for(uint32_t x=100;x<500;x+=20) far_floor=std::max(far_floor,std::abs(red(off,x,200)-red(on,x,200)));
            std::fprintf(stderr,"shadows: floor shadow %d, far floor %d, shadow pass runs %llu\n",shadow,far_floor,(unsigned long long)renderer.pass_runs(uc::GpuPass::shadow));
            CHECK(shadow>=25 && far_floor<=1 && renderer.pass_runs(uc::GpuPass::shadow)>0);
            for(const auto level:{uc::EffectLevel::low,uc::EffectLevel::high,uc::EffectLevel::ultra}) {   // every preset shadows the floor and leaves the far floor alone
                frame(level,true); frame(level,true);
                const auto shaded=frame(level,true);
                int dark=0, distant=0;
                for(uint32_t y=262;y<340;++y) for(uint32_t x=640;x<900;++x) dark=std::max(dark,red(off,x,y)-red(shaded,x,y));
                for(uint32_t x=100;x<500;x+=20) distant=std::max(distant,std::abs(red(off,x,200)-red(shaded,x,200)));
                std::fprintf(stderr,"shadows %s: floor shadow %d, far floor %d\n",std::string(uc::effect_level_name(level)).c_str(),dark,distant);
                CHECK(dark>=25 && distant<=1);
            }
            frame(uc::EffectLevel::off,false); frame(uc::EffectLevel::off,false); const auto point_off=frame(uc::EffectLevel::off,false);
            frame(uc::EffectLevel::medium,false); frame(uc::EffectLevel::medium,false);
            CHECK(frame(uc::EffectLevel::medium,false).rgba==point_off.rgba);   // a point light is no shadow caster: nothing changes (Review Focus 1)
            frame(uc::EffectLevel::medium,true); frame(uc::EffectLevel::medium,true);
            const auto runs=renderer.pass_runs(uc::GpuPass::shadow);
            const auto menu=frame(uc::EffectLevel::medium,true,true);   // no scene this frame: no shadow pass (Review Focus 2)
            CHECK(renderer.pass_runs(uc::GpuPass::shadow)==runs && red(menu,100,100)==16);
            frame(uc::EffectLevel::medium,true); frame(uc::EffectLevel::medium,true);
            auto before_pools=renderer.descriptor_pool_creations(), before_textures=renderer.texture_creations();
            for(int i=0;i<10;++i) frame(uc::EffectLevel::medium,true);
            const auto steady_pools=renderer.descriptor_pool_creations()-before_pools, steady_textures=renderer.texture_creations()-before_textures;
            before_pools=renderer.descriptor_pool_creations(); before_textures=renderer.texture_creations();
            for(int i=0;i<20;++i) frame(i%2 ? uc::EffectLevel::low : uc::EffectLevel::medium,true);   // a preset change swaps the map, not the per-frame resources
            CHECK(renderer.descriptor_pool_creations()-before_pools<=2*steady_pools+2 && renderer.texture_creations()-before_textures<=2*steady_textures+2);
            renderer.force_lights(false);
            frame(uc::EffectLevel::off,true); frame(uc::EffectLevel::off,true);
        }
        if (wanted("ppl")) {
            // Per-pixel lighting: a GX point light in front of a quad facing the camera. Per vertex the light reaches every corner equally (a flat dim quad);
            // per pixel the centre is lit by the near light. Baked-colour (unlit) draws stay identical.
            CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
            auto frame=[&](bool per_pixel) {
                uc::GraphicsEffects effects; effects.anisotropy=1; effects.per_pixel_lighting=per_pixel; renderer.set_effects(effects);
                gx::FramePacket packet; packet.present=true; packet.display_source={0,0,640,360};
                auto lit=quad(0x3333,-1,-1,1,1,{255,255,255,255},true);
                for(auto& vertex:lit.vertices) vertex.normal={0,0,1,0,0,0,0,0,0};
                auto& uniforms=lit.uniforms.edit();
                uniforms.channels[0]=1|2|(1<<2)|(2<<7)|(3<<9);   // vertex colour as material, lighting on, light 0, clamped diffuse, distance attenuation
                auto& light=uniforms.lights[0];
                light.color={255,255,255,255}; light.cosine={1,0,0,0}; light.distance={1,0,4,0}; light.position={0,0,-0.8f,0}; light.direction={0,0,1,0};
                packet.draws.push_back(std::move(lit));
                auto baked=quad(0x4444,0.5f,-0.9f,0.9f,-0.5f,{200,100,50,255},true);
                for(auto& vertex:baked.vertices) vertex.normal={0,0,1,0,0,0,0,0,0};
                packet.draws.push_back(std::move(baked));
                return renderer.capture(submit(std::move(packet)));
            };
            const auto off=frame(false), on=frame(true);
            const auto value=[&](const uc::RenderPixels& pixels,uint32_t x,uint32_t y,int channel=0) { return int(pixels.rgba[(size_t(y)*pixels.width+x)*4+channel]); };
            std::fprintf(stderr,"ppl: centre off %d on %d, corner off %d on %d\n",value(off,640,360),value(on,640,360),value(off,20,20),value(on,20,20));
            CHECK(value(on,640,360)>=value(off,640,360)+40);   // the light is near the centre
            CHECK(std::abs(value(on,20,20)-value(off,20,20))<=4);   // the corners are where the vertices are: the same light there
            for(int channel=0;channel<3;++channel) CHECK(value(on,1100,100,channel)==value(off,1100,100,channel));   // the baked-colour quad is unchanged
        }
        if (wanted("blur")) {
            // A red quad moves 40 internal pixels between frames: its edge smears along the motion; a still scene and a camera cut (field of view jump) stay as drawn.
            CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
            auto frame=[&](uc::EffectLevel level,float shift,float zoom) {
                uc::GraphicsEffects effects; effects.anisotropy=1; effects.motion_blur=level; renderer.set_effects(effects);
                gx::FramePacket scene; scene.present=false;
                auto red=quad(0x1234,-1,-1,0.1f,1,{255,0,0,255},true);
                red.transforms.edit().position_rows[0][3]=shift;
                red.transforms.edit().projection[0][0]=zoom;
                scene.draws.push_back(std::move(red));
                submit(std::move(scene));
                gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                submit(std::move(boundary));
                gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                submit(std::move(empty));
                gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                return renderer.capture(submit(std::move(hud)));
            };
            const auto moved=[&](uc::EffectLevel level) { frame(level,0,1); frame(level,0,1); return frame(level,0.0625f,1); };
            const auto distinct=[&](const uc::RenderPixels& pixels) {   // intermediate red values along the middle row
                std::set<int> values;
                for(uint32_t x=0;x<pixels.width;++x) { const int red=pixels.rgba[(size_t(360)*pixels.width+x)*4]; if(red>20 && red<250) values.insert(red); }
                return values.size();
            };
            const auto off=moved(uc::EffectLevel::off), on=moved(uc::EffectLevel::high);
            std::fprintf(stderr,"blur: distinct intermediate values off %zu, on %zu\n",distinct(off),distinct(on));
            CHECK(distinct(off)<=2 && distinct(on)>=8);
            CHECK(renderer.pass_runs(uc::GpuPass::motion_blur)>0);
            frame(uc::EffectLevel::off,0.0625f,1); const auto still_off=frame(uc::EffectLevel::off,0.0625f,1);
            frame(uc::EffectLevel::high,0.0625f,1); const auto still_on=frame(uc::EffectLevel::high,0.0625f,1);
            CHECK(still_off.rgba==still_on.rgba);   // no motion: nothing to smear
            frame(uc::EffectLevel::off,0,1); const auto cut_off=frame(uc::EffectLevel::off,0.0625f,2);
            frame(uc::EffectLevel::high,0,1); const auto cut_on=frame(uc::EffectLevel::high,0.0625f,2);
            CHECK(cut_off.rgba==cut_on.rgba);   // a camera cut skips the blur
        }
        if (wanted("rt-material")) {
            // The albedo and material-record targets of the RT effects: a textured, lit box on a floor. With an RT option active every depth-writing scene
            // pixel names its draw's material record and holds stage-0 texture x material colour; with RT off the targets do not exist and the picture is the same.
            if (!renderer.ray_tracing_supported()) std::fprintf(stderr,"rt-material: skipped, the device has no ray tracing\n");
            else {
                CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
                auto solid=std::make_shared<gx::TexturePixels>();
                solid->levels.push_back({4,4,{}});
                for(int i=0;i<16;++i) solid->levels[0].rgba.insert(solid->levels[0].rgba.end(),{200,100,50,255});
                const std::array<float,4> material{0.8f,0.6f,0.4f,1.f};
                const auto surface=[&](std::array<std::array<float,3>,4> corners,std::array<float,3> normal,bool box) {
                    auto draw=quad(0x1000,-1,-1,1,1,{180,180,180,255},true);
                    for(size_t i=0;i<4;++i) { draw.vertices[i].position=corners[i]; draw.vertices[i].normal={normal[0],normal[1],normal[2],0,0,0,0,0,0}; }
                    draw.transforms.edit().projection={{{1,0,0,0},{0,1,0,0},{0,0,-100.f/99,-100.f/99},{0,0,-1,0}}};
                    auto& uniforms=draw.uniforms.edit();
                    uniforms.pixel[1]=1u|(3u<<1)|(1u<<4);   // depth test "less or equal" with update
                    uniforms.channels[0]=2|(1<<2)|(2<<7);   // material register, lighting on, light 0, clamped diffuse
                    uniforms.materials[0]=material; uniforms.ambient[0]={0.35f,0.35f,0.35f,1};
                    uniforms.lights[0].color={255,255,255,255}; uniforms.lights[0].direction={0,0,1,0}; uniforms.lights[0].position={-1e6f,1e6f,1e6f,0};
                    uniforms.lights[0].distance={1,0,0,0}; uniforms.lights[0].cosine={1,0,0,0};
                    if(box) {   // stage 0: texture x rasterised colour
                        draw.texture=solid; draw.texture_wrap[0]=0x95; draw.material.stages=1; draw.material.swaps={0xE4,0xE4,0xE4,0xE4};
                        draw.material.stage[0]={0x8F8AF,0x8FFC0,64,0,0};
                    }
                    return draw;
                };
                auto frame=[&](bool rt) {
                    uc::GraphicsEffects effects; effects.anisotropy=1; effects.ray_tracing=rt; effects.rt_shadows=uc::EffectLevel::medium; renderer.set_effects(effects);
                    gx::FramePacket scene; scene.present=false;
                    scene.draws.push_back(surface({{{-12,-1,-2},{12,-1,-2},{12,-1,-8},{-12,-1,-8}}},{0,1,0},false));   // the floor (no TEV stage: rasterised x texture 0)
                    scene.draws.push_back(surface({{{-0.5f,-1,-4},{0.5f,-1,-4},{0.5f,-0.4f,-4},{-0.5f,-0.4f,-4}}},{0,0,1},true));   // the box's front
                    submit(std::move(scene));
                    gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                    auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                    boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                    submit(std::move(boundary));
                    gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                    submit(std::move(empty));
                    gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                    hud.draws.push_back(quad(0x5678,0.6f,-0.9f,0.9f,-0.6f,{255,0,0,255},false));
                    return renderer.capture(submit(std::move(hud)));
                };
                frame(false); const auto off=frame(false);   // the first frame decides scene mode for the next one
                CHECK(!renderer.rt_material_targets());
                frame(true); const auto on=frame(true);
                CHECK(renderer.rt_material_targets());
                const auto targets=renderer.capture_rt_material();
                const auto records=renderer.rt_material_records();
                CHECK(targets.width==1280 && targets.height==960);
                const auto at=[&](uint32_t x,uint32_t y) { return targets.records[size_t(y)*targets.width+x]; };
                const uint32_t box=at(640,297), floor=at(640,200), empty=at(640,50);   // the box's front, the floor behind it, nothing drawn
                const size_t i=(size_t(297)*targets.width+640)*4;
                std::fprintf(stderr,"rt-material: records %zu, box %u floor %u empty %u; box albedo %u %u %u\n",records.size(),box,floor,empty,targets.albedo[i],targets.albedo[i+1],targets.albedo[i+2]);
                CHECK(box<records.size() && floor<records.size() && box!=floor && empty==~0u);
                CHECK(records[box].material_color==material && (records[box].flags&uc::kRtModelValid) && records[box].light_mask==1);
                CHECK(records[floor].flags&uc::kRtModelValid);   // no TEV stage: the renderer draws rasterised x texture 0, the modelled form
                CHECK(std::abs(int(targets.albedo[i])-160)<=1 && std::abs(int(targets.albedo[i+1])-60)<=1 && std::abs(int(targets.albedo[i+2])-20)<=1);   // texture x material colour, unlit
                CHECK(on.rgba==off.rgba);   // writing the targets leaves the picture alone
                frame(false); frame(false);
                CHECK(!renderer.rt_material_targets());
            }
        }
        if (wanted("rt-as")) {
            // Per-frame acceleration structures: a box on a floor under a directional light as mesh draws, with RT shadows enabled through the effective block.
            if (!renderer.ray_tracing_supported()) std::fprintf(stderr,"rt-as: skipped, the device has no ray tracing\n");
            else {
                CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
                using Quad=std::array<std::array<float,3>,4>;
                const auto box=[](float shift) {
                    std::vector<Quad> faces{{{{-0.5f,-0.4f,-5},{0.5f,-0.4f,-5},{0.5f,-0.4f,-4},{-0.5f,-0.4f,-4}}},{{{-0.5f,-1,-4},{0.5f,-1,-4},{0.5f,-0.4f,-4},{-0.5f,-0.4f,-4}}},{{{0.5f,-1,-4},{0.5f,-1,-5},{0.5f,-0.4f,-5},{0.5f,-0.4f,-4}}}};
                    for(auto& face:faces) for(auto& corner:face) corner[0]+=shift;
                    return faces;
                };
                const Quad floor_quad{{{-12,-1,-2},{12,-1,-2},{12,-1,-8},{-12,-1,-8}}};
                const gx::Projection projection{{{1,0,0,0},{0,1,0,0},{0,0,-100.f/99,-100.f/99},{0,0,-1,0}}};
                const std::array<float,3> light{-1e6f,1e6f,1e6f};
                const auto surface=[&](const Quad& corners,bool mesh,bool skinned=false) {   // only skinned draws (per-vertex matrices) occlude: the box is skinned, the floor is not
                    auto draw=quad(0x1000,-1,-1,1,1,{180,180,180,255},true);
                    if(skinned) draw.state.edit().descriptors[0]=gx::Mode::direct;
                    for(size_t i=0;i<4;++i) { draw.vertices[i].position=corners[i]; draw.vertices[i].normal={0,1,0,0,0,0,0,0,0}; }
                    draw.transforms.edit().projection=projection;
                    auto& uniforms=draw.uniforms.edit();
                    uniforms.pixel[1]=1u|(3u<<1)|(1u<<4);   // depth test "less or equal" with update (quad() compares "always"): the nearest surface is what the CPU reference sees
                    uniforms.channels[0]=1|2|(1<<2)|(2<<7);
                    uniforms.ambient[0]={90,90,90,255};
                    uniforms.lights[0].color={255,255,255,255}; uniforms.lights[0].direction={0,0,1,0};
                    uniforms.lights[0].position={light[0],light[1],light[2],0};
                    uniforms.lights[0].distance={1,0,0,0}; uniforms.lights[0].cosine={1,0,0,0};
                    if(mesh) { draw.mesh=std::make_shared<const gx::MeshBuffers>(gx::MeshBuffers{draw.vertices,draw.indices}); draw.vertices.clear(); draw.indices.clear(); }
                    return draw;
                };
                // menu: no scene this frame; meshes=false: a scene whose draws are all inline (no instance at all)
                auto frame=[&](float shift,bool menu=false,bool meshes=true) {
                    gx::FramePacket scene; scene.present=false;
                    if(!menu) {
                        scene.draws.push_back(surface(floor_quad,meshes));
                        for(const auto& face:box(shift)) scene.draws.push_back(surface(face,meshes,true));
                        submit(std::move(scene));
                        gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                        auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                        boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                        submit(std::move(boundary));
                        gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                        submit(std::move(empty));
                    }
                    gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                    hud.draws.push_back(quad(0x5678,0.6f,-0.9f,0.9f,-0.6f,{255,0,0,255},false));
                    submit(std::move(hud));
                };
                // The ray-traced shadows build the structures; the shadow values themselves are checked by --rt-shadows.
                const auto set_rt=[&](bool on) { uc::GraphicsEffects rt_effects; rt_effects.anisotropy=1; rt_effects.ray_tracing=on; rt_effects.rt_shadows=uc::EffectLevel::medium; renderer.set_effects(rt_effects); };
                set_rt(true);
                frame(0); frame(0);   // the first frame decides scene mode for the next one
                const auto stats=renderer.rt_stats();
                std::fprintf(stderr,"rt-as: %u BLAS, %u geometries, %llu triangles; as %.3f ms\n",stats.blas,stats.geometries,(unsigned long long)stats.triangles,renderer.pass_ms(uc::GpuPass::rt_as));
                CHECK(stats.blas==1 && stats.skinned_blas==1 && stats.static_instances==1 && stats.instances==2 && stats.geometries==3 && stats.triangles==6);   // the three skinned box faces: one per-frame BLAS; the floor (static): an instance of a cached BLAS
                frame(30); frame(30);   // the box far right: the structures follow the geometry
                CHECK(renderer.rt_stats().blas==1 && renderer.rt_stats().instances==2 && renderer.rt_stats().triangles==6);
                auto as_runs=renderer.pass_runs(uc::GpuPass::rt_as), shadow_runs=renderer.pass_runs(uc::GpuPass::rt_shadows);
                frame(0,true);   // no scene this frame: nothing built, no shadows (Review Focus 1)
                CHECK(renderer.rt_stats().blas==0 && renderer.pass_runs(uc::GpuPass::rt_as)==as_runs && renderer.pass_runs(uc::GpuPass::rt_shadows)==shadow_runs);
                frame(0,false,false); frame(0,false,false);   // a scene without a single instance (inline draws only)
                CHECK(renderer.rt_stats().blas==0 && renderer.pass_runs(uc::GpuPass::rt_as)==as_runs && renderer.pass_runs(uc::GpuPass::rt_shadows)==shadow_runs);
                for(int i=0;i<8;++i) frame(0);   // every frame-in-flight slot has built once
                const auto growths=renderer.rt_buffer_growths();
                for(int i=0;i<20;++i) { set_rt(i%2==0); frame(0); frame(0); }   // the master switch toggled mid-session: no stale structure, no new buffers (Review Focus 2, 4)
                std::fprintf(stderr,"rt-as: buffer growths %llu, after 20 toggles %llu\n",(unsigned long long)growths,(unsigned long long)renderer.rt_buffer_growths());
                CHECK(renderer.rt_buffer_growths()==growths);
                set_rt(false);
                frame(0); frame(0);
                as_runs=renderer.pass_runs(uc::GpuPass::rt_as); shadow_runs=renderer.pass_runs(uc::GpuPass::rt_shadows);
                frame(0); frame(0);   // the master switch off: nothing RT runs
                CHECK(renderer.pass_runs(uc::GpuPass::rt_as)==as_runs && renderer.pass_runs(uc::GpuPass::rt_shadows)==shadow_runs);
            }
        }
        if (wanted("rt-shadows")) {
            // Ray-traced shadows (rt_shadow_pass.h, plan p2 Tasks 8-9): a thin plate (the box's front) over a floor, lit by one directional or one point light. The
            // visibility signal must equal shadow_factor() computed on the CPU from an independent ray cast of the plate, and the frame must show it.
            if (!renderer.ray_tracing_supported()) std::fprintf(stderr,"rt-shadows: skipped, the device has no ray tracing\n");
            else {
                CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
                renderer.set_rt_trace_lamps(true);   // the game's point lights are camera-relative and not traced in play; these fixtures check the machinery (the overhead check turns it off)
                using Vec=std::array<double,3>;
                const gx::Projection projection{{{1,0,0,0},{0,1,0,0},{0,0,-100.f/99,-100.f/99},{0,0,-1,0}}};
                const std::array<float,4> material{0.8f,0.6f,0.4f,1.f};
                std::array<float,4> ambient{0.35f,0.35f,0.35f,1.f};   // the fixtures' ambient register (the saturation check raises it)
                const std::array<std::array<float,3>,4> floor_corners{{{-12,-1,-2},{12,-1,-2},{12,-1,-8},{-12,-1,-8}}}, plate_corners{{{-0.5f,-1,-4},{0.5f,-1,-4},{0.5f,-0.4f,-4},{-0.5f,-0.4f,-4}}};
                enum class Light { directional, point };
                bool dual=false;   // light 0 a rig (directional), light 1 a lamp (point), both in the record's mask; dimmer lights so that the GX clamp at 1 does not hide the shares
                auto light_uniform=[&](Light kind) {
                    gx::LightUniform light;
                    const float level=dual ? 110.f : 255.f;
                    light.color={level,level,level,255}; light.direction={0,0,1,0}; light.cosine={1,0,0,0}; light.distance={1,0,0,0};
                    light.position=kind==Light::directional ? std::array<float,4>{-1e6f,1e6f,1e6f,0} : std::array<float,4>{1.5f,1.5f,-3.f,0};
                    return light;
                };
                float shell=0.f;   // half size of a static box around the point light (its fixture), 0 = none
                bool vertical_plate=true, roof=false;   // the standing plate of the fixture; a horizontal plate above the floor (the overhead light's occluder)
                float roof_y=0.2f;
                const auto roof_corners=[&]() { return std::array<std::array<float,3>,4>{{{-1,roof_y,-5},{1,roof_y,-5},{1,roof_y,-3},{-1,roof_y,-3}}}; };
                bool plate_skinned=true;   // only skinned draws (per-vertex matrices: characters) occlude; the game's static level geometry does not (see below)
                auto surface=[&](const std::array<std::array<float,3>,4>& corners,std::array<float,3> normal,bool plate,Light kind,bool lit) {
                    auto draw=quad(0x1000,-1,-1,1,1,{180,180,180,255},true);
                    if(plate && plate_skinned) draw.state.edit().descriptors[0]=gx::Mode::direct;
                    for(size_t i=0;i<4;++i) { draw.vertices[i].position=corners[i]; draw.vertices[i].normal={normal[0],normal[1],normal[2],0,0,0,0,0,0}; }
                    draw.transforms.edit().projection=projection;
                    auto& uniforms=draw.uniforms.edit();
                    uniforms.pixel[1]=1u|(3u<<1)|(1u<<4);   // depth test "less or equal" with update
                    uniforms.channels[0]=lit ? 2|(1<<2)|(2<<7)|(kind==Light::point ? 3<<9 : 0) : 0;   // material register, lighting on, light 0, clamped diffuse, distance attenuation for the point light
                    uniforms.materials[0]=material; uniforms.ambient[0]=ambient;
                    uniforms.lights[0]=light_uniform(kind);
                    if(dual && lit) { uniforms.lights[0]=light_uniform(Light::directional); uniforms.lights[1]=light_uniform(Light::point); uniforms.channels[0]=2|(1<<2)|(1<<3)|(2<<7)|(3<<9); }
                    if(plate) { draw.material.stages=1; draw.material.swaps={0xE4,0xE4,0xE4,0xE4}; draw.material.stage[0]={0x8F8AF,0x8FFC0,64,0,0}; }   // stage 0: texture x rasterised colour (the model-valid form)
                    draw.mesh=std::make_shared<const gx::MeshBuffers>(gx::MeshBuffers{draw.vertices,draw.indices}); draw.vertices.clear(); draw.indices.clear();   // an RT instance needs a mesh draw
                    return draw;
                };
                struct Setting { bool master; uc::EffectLevel sun, local, raster; uc::EffectLevel ao=uc::EffectLevel::off, gtao=uc::EffectLevel::off; };   // ao: rt_ambient_occlusion, gtao: the raster ambient occlusion
                bool wall=false;   // a wall across the floor at z = -5 facing the camera (the crease of the ambient occlusion check)
                auto frame=[&](Setting s,Light kind,bool lit=true,float zoom=1.f) {
                    uc::GraphicsEffects effects; effects.anisotropy=1; effects.ray_tracing=s.master; effects.rt_shadows=s.sun; effects.rt_local_shadows=s.local; effects.shadows=s.raster; effects.rt_ambient_occlusion=s.ao; effects.ambient_occlusion=s.gtao; renderer.set_effects(effects);
                    gx::FramePacket scene; scene.present=false;
                    auto floor=surface(floor_corners,{0,1,0},false,kind,lit); floor.transforms.edit().projection[0][0]=zoom;
                    if(wall) { auto draw=surface({{{-12,-1,-2.8f},{12,-1,-2.8f},{12,3,-2.8f},{-12,3,-2.8f}}},{0,0,1},true,kind,lit); draw.transforms.edit().projection[0][0]=zoom; scene.draws.push_back(std::move(draw)); }
                    auto plate=surface(plate_corners,{0,0,1},true,kind,lit); plate.transforms.edit().projection[0][0]=zoom;
                    scene.draws.push_back(std::move(floor)); if(vertical_plate) scene.draws.push_back(std::move(plate));
                    if(roof) { auto draw=surface(roof_corners(),{0,-1,0},true,kind,lit); draw.state.edit().descriptors[0]=gx::Mode::none; draw.transforms.edit().projection[0][0]=zoom; scene.draws.push_back(std::move(draw)); }   // static: a level ceiling
                    if(shell>0) {   // a static box around the point light (its fixture): six faces
                        const float cx=1.5f, cy=1.5f, cz=-3.f, h=shell;
                        const std::array<std::array<std::array<float,3>,4>,6> faces{{
                            {{{cx-h,cy-h,cz-h},{cx+h,cy-h,cz-h},{cx+h,cy+h,cz-h},{cx-h,cy+h,cz-h}}}, {{{cx-h,cy-h,cz+h},{cx+h,cy-h,cz+h},{cx+h,cy+h,cz+h},{cx-h,cy+h,cz+h}}},
                            {{{cx-h,cy-h,cz-h},{cx-h,cy-h,cz+h},{cx-h,cy+h,cz+h},{cx-h,cy+h,cz-h}}}, {{{cx+h,cy-h,cz-h},{cx+h,cy-h,cz+h},{cx+h,cy+h,cz+h},{cx+h,cy+h,cz-h}}},
                            {{{cx-h,cy-h,cz-h},{cx+h,cy-h,cz-h},{cx+h,cy-h,cz+h},{cx-h,cy-h,cz+h}}}, {{{cx-h,cy+h,cz-h},{cx+h,cy+h,cz-h},{cx+h,cy+h,cz+h},{cx-h,cy+h,cz+h}}}}};
                        for(const auto& face:faces) { auto draw=surface(face,{0,0,1},false,kind,lit); draw.transforms.edit().projection[0][0]=zoom; scene.draws.push_back(std::move(draw)); }
                    }
                    submit(std::move(scene));
                    gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                    auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                    boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                    submit(std::move(boundary));
                    gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                    submit(std::move(empty));
                    gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                    hud.draws.push_back(quad(0x5678,0.6f,-0.9f,0.9f,-0.6f,{255,0,0,255},false));
                    return renderer.capture(submit(std::move(hud)));
                };
                // The CPU reference: the floor point P under the light, the plate as the only occluder, the GX equations of rt_shadow_math.h.
                const auto pixel_of=[&](const Vec& p) { return std::array<uint32_t,2>{uint32_t((p[0]/-p[2]+1)/2*1280),uint32_t((p[1]/-p[2]+1)/2*720)}; };   // unit scales, no offsets; the 1280x720 scene region
                const auto blocked=[&](const Vec& p,Light kind) {
                    const auto l=light_uniform(kind);
                    const Vec target{kind==Light::directional ? p[0]+(l.position[0]/1.7320508e6)*1e4 : l.position[0],kind==Light::directional ? p[1]+(l.position[1]/1.7320508e6)*1e4 : l.position[1],kind==Light::directional ? p[2]+(l.position[2]/1.7320508e6)*1e4 : l.position[2]};
                    const double t=(-4-p[2])/(target[2]-p[2]);
                    if(!(t>0 && t<1)) return false;
                    const double x=p[0]+(target[0]-p[0])*t, y=p[1]+(target[1]-p[1])*t;
                    return x>=-0.5 && x<=0.5 && y>=-1 && y<=-0.4;
                };
                const auto expected=[&](const Vec& p,Light kind) {
                    const float contribution=uc::gx_light_contribution(light_uniform(kind),kind==Light::point ? 3 : 0,2,{float(p[0]),float(p[1]),float(p[2])},{0,1,0});
                    const float visibility[]={blocked(p,kind) ? 0.f : 1.f};
                    return uc::shadow_factor(ambient[0],std::array<float,1>{contribution},visibility);
                };
                const auto signal_at=[&](const uc::VulkanRenderer::RtShadowPixels& signal,const Vec& p,uint32_t divisor) {   // the point's signal pixel (the point's shadow can be only a few pixels tall)
                    const auto pixel=pixel_of(p);
                    return double(signal.factor[size_t(pixel[1]/divisor)*signal.width+(pixel[0]/divisor)]);
                };
                // Sample points on the floor that the camera sees (not behind the plate) and whose shadow state is the same for every point within 8 cm (a robust margin).
                const auto hidden=[&](const Vec& p) { const auto at=pixel_of(p); return at[0]>=545 && at[0]<=735 && at[1]>=260 && at[1]<=335; };
                const auto robust=[&](const Vec& p,Light kind,bool want_blocked,double margin) {
                    for(const double dx:{-margin,0.0,margin}) for(const double dz:{-margin,0.0,margin}) if(blocked({p[0]+dx,p[1],p[2]+dz},kind)!=want_blocked) return false;
                    return true;
                };
                const auto pick=[&](Light kind,bool want_blocked) {   // the widest margin any visible point has: the middle of the region, away from every edge
                    for(const double margin:{0.25,0.18,0.12,0.08}) {
                        std::vector<Vec> found;
                        for(double x=-4;x<=4;x+=0.05) for(double z=-5.5;z<=-4.2;z+=0.05) { const Vec p{x,-1,z}; if(!hidden(p) && robust(p,kind,want_blocked,margin)) found.push_back(p); }
                        if(!found.empty()) return found[found.size()/2];
                    }
                    throw std::runtime_error("rt-shadows: no sample point");
                };
                const Vec shadowed_sun=pick(Light::directional,true), lit_sun=pick(Light::directional,false), shadowed_point=pick(Light::point,true), lit_point=pick(Light::point,false);
                std::fprintf(stderr,"rt-shadows: expected sun shadowed %.3f lit %.3f, point shadowed %.3f lit %.3f\n",expected(shadowed_sun,Light::directional),expected(lit_sun,Light::directional),expected(shadowed_point,Light::point),expected(lit_point,Light::point));
                CHECK(blocked(shadowed_sun,Light::directional) && !blocked(lit_sun,Light::directional) && blocked(shadowed_point,Light::point) && !blocked(lit_point,Light::point));   // the fixture's geometry
                const Setting off{false,uc::EffectLevel::off,uc::EffectLevel::off,uc::EffectLevel::off};
                const Setting sun_only{true,uc::EffectLevel::medium,uc::EffectLevel::off,uc::EffectLevel::off}, local_only{true,uc::EffectLevel::off,uc::EffectLevel::medium,uc::EffectLevel::off};
                // (a) the sun: the signal against the CPU reference
                frame(sun_only,Light::directional); frame(sun_only,Light::directional);   // the first frame decides scene mode for the next one
                auto signal=renderer.capture_rt_shadow_signal();
                CHECK(signal.width==640 && signal.height==360);   // medium: half the 1280x720 scene region
                const double sun_shadow=signal_at(signal,shadowed_sun,2), sun_lit=signal_at(signal,lit_sun,2);
                std::fprintf(stderr,"rt-shadows: sun signal shadowed %.3f lit %.3f\n",sun_shadow,sun_lit);
                CHECK(std::abs(sun_shadow-expected(shadowed_sun,Light::directional))<=0.05 && std::abs(sun_lit-1.0)<=0.01);
                // (a2) GX lights are art-directed rigs that ignore walls and ceilings: a static (non-skinned) plate must not occlude, or every character lit from above would be black indoors
                plate_skinned=false;
                frame(sun_only,Light::directional); frame(sun_only,Light::directional);
                signal=renderer.capture_rt_shadow_signal();
                CHECK(std::abs(signal_at(signal,shadowed_sun,2)-1.0)<=0.01);
                // (a3) the level's own lamps are in the level: a static plate does occlude a lamp (rt_local_shadows), the lamp's ray sees the whole scene
                frame(local_only,Light::point); frame(local_only,Light::point);
                signal=renderer.capture_rt_shadow_signal();
                const double static_lamp=signal_at(signal,shadowed_point,2);
                std::fprintf(stderr,"rt-shadows: static plate under a lamp: signal %.3f, expected %.3f\n",static_lamp,expected(shadowed_point,Light::point));
                CHECK(expected(shadowed_point,Light::point)<0.6 && std::abs(static_lamp-expected(shadowed_point,Light::point))<=0.05);
                // (a4) a lamp sits inside its fixture: a shell smaller than the clearance around it is ignored (the plate's shadow stays, the lit floor stays lit); a larger one blocks the lamp (the documented limit)
                shell=0.4f*uc::kRtLampClearance;
                frame(local_only,Light::point); frame(local_only,Light::point);
                signal=renderer.capture_rt_shadow_signal();
                std::fprintf(stderr,"rt-shadows: small shell: shadowed %.3f lit %.3f\n",signal_at(signal,shadowed_point,2),signal_at(signal,lit_point,2));
                CHECK(std::abs(signal_at(signal,shadowed_point,2)-expected(shadowed_point,Light::point))<=0.05 && std::abs(signal_at(signal,lit_point,2)-1.0)<=0.01);
                shell=1.5f*uc::kRtLampClearance;
                frame(local_only,Light::point); frame(local_only,Light::point);
                signal=renderer.capture_rt_shadow_signal();
                std::fprintf(stderr,"rt-shadows: large shell: lit %.3f\n",signal_at(signal,lit_point,2));
                CHECK(signal_at(signal,lit_point,2)<0.8);
                shell=0.f;
                plate_skinned=true;
                frame(sun_only,Light::directional); frame(sun_only,Light::directional);
                signal=renderer.capture_rt_shadow_signal();
                // (b) the point light is a local light: it is traced by rt_local_shadows only
                frame(local_only,Light::point); frame(local_only,Light::point);
                signal=renderer.capture_rt_shadow_signal();
                const double point_shadow=signal_at(signal,shadowed_point,2), point_lit=signal_at(signal,lit_point,2);
                std::fprintf(stderr,"rt-shadows: point signal shadowed %.3f lit %.3f\n",point_shadow,point_lit);
                CHECK(std::abs(point_shadow-expected(shadowed_point,Light::point))<=0.05 && std::abs(point_lit-1.0)<=0.01);
                frame(sun_only,Light::point); frame(sun_only,Light::point);   // the sun level does not trace a point light
                signal=renderer.capture_rt_shadow_signal();
                CHECK(std::abs(signal_at(signal,shadowed_point,2)-1.0)<=0.01);
                // (b2) GX clamps ambient + light at 1: with a bright ambient the shadow takes only what the clamp leaves (0.9 / 1 instead of the unclamped 0.61)
                ambient={0.9f,0.9f,0.9f,1.f};
                frame(sun_only,Light::directional); frame(sun_only,Light::directional);
                signal=renderer.capture_rt_shadow_signal();
                const double bright_shadow=signal_at(signal,shadowed_sun,2);
                std::fprintf(stderr,"rt-shadows: bright ambient: shadowed %.3f, expected %.3f\n",bright_shadow,expected(shadowed_sun,Light::directional));
                CHECK(std::abs(bright_shadow-expected(shadowed_sun,Light::directional))<=0.05 && bright_shadow>0.8);
                ambient={0.35f,0.35f,0.35f,1.f};
                // (b3) light classes: one record with a rig (directional) and a lamp (point): rt_shadows removes only the rig's share, rt_local_shadows only the lamp's
                dual=true; ambient={0.1f,0.1f,0.1f,1.f};
                const auto expected_dual=[&](const Vec& p,bool rig_traced,bool lamp_traced) {
                    const std::array<float,2> contributions{uc::gx_light_contribution(light_uniform(Light::directional),3,2,{float(p[0]),float(p[1]),float(p[2])},{0,1,0}),
                                                            uc::gx_light_contribution(light_uniform(Light::point),3,2,{float(p[0]),float(p[1]),float(p[2])},{0,1,0})};
                    const std::array<float,2> visibility{rig_traced && blocked(p,Light::directional) ? 0.f : 1.f,lamp_traced && blocked(p,Light::point) ? 0.f : 1.f};
                    return uc::shadow_factor(ambient[0],contributions,visibility);
                };
                for(const auto& [name,setting,rig_traced,lamp_traced]:{std::tuple{"rig only",sun_only,true,false},std::tuple{"lamp only",local_only,false,true},std::tuple{"both",Setting{true,uc::EffectLevel::medium,uc::EffectLevel::medium,uc::EffectLevel::off},true,true}}) {
                    frame(setting,Light::directional); frame(setting,Light::directional);
                    signal=renderer.capture_rt_shadow_signal();
                    for(const Vec& p:{shadowed_sun,shadowed_point}) {
                        const double got=signal_at(signal,p,2), want=expected_dual(p,rig_traced,lamp_traced);
                        std::fprintf(stderr,"rt-shadows: dual %s at (%.2f %.2f %.2f): signal %.3f, expected %.3f\n",name,p[0],p[1],p[2],got,want);
                        CHECK(std::abs(got-want)<=0.05);
                    }
                }
                CHECK(blocked(shadowed_sun,Light::directional) && blocked(shadowed_point,Light::point));   // the two points exercise a blocked rig and a blocked lamp
                dual=false; ambient={0.35f,0.35f,0.35f,1.f};
                // (b4) the synthetic key light (rt_shadow_math.h): under rt_local_shadows a static roof above the floor shadows it by the softened strength; the game's point
                // lights are not traced (camera-relative in the game); the sun level alone does not run the key light
                {
                    renderer.set_rt_trace_lamps(false);
                    vertical_plate=false; roof=true;
                    const auto& toward=uc::kRtOverheadDirection;
                    const double reach=1.2/toward[1];   // from the floor (y = -1) up to the roof (y = 0.2) along the key light's direction
                    const Vec under{-1.0,-1,-4.5}, aside{3.0,-1,-4.5};
                    const auto inside=[&](const Vec& p) { const double x=p[0]+toward[0]*reach, z=p[2]+toward[2]*reach; return x>=-1 && x<=1 && z>=-5 && z<=-3; };
                    CHECK(inside(under) && !inside(aside));
                    frame(local_only,Light::point); frame(local_only,Light::point);
                    signal=renderer.capture_rt_shadow_signal();
                    const double roofed=signal_at(signal,under,2), open=signal_at(signal,aside,2);
                    std::fprintf(stderr,"rt-shadows: key light under a roof %.3f (expected %.3f), in the open %.3f\n",roofed,1.0-uc::kRtOverheadStrength,open);
                    CHECK(std::abs(roofed-(1.0-uc::kRtOverheadStrength))<=0.05 && std::abs(open-1.0)<=0.01);   // the point lamp's roof-blocked share is not removed: lamps are not traced
                    frame(sun_only,Light::point); frame(sun_only,Light::point);
                    signal=renderer.capture_rt_shadow_signal();
                    CHECK(std::abs(signal_at(signal,under,2)-1.0)<=0.01);   // rt_local_shadows is off: no key light
                    // a roof beyond the light's reach (the car's own roof above a seat) does not shadow the floor under it
                    roof_y=6.f;
                    const double far_reach=(roof_y+1.0)/toward[1];
                    CHECK(far_reach>uc::kRtOverheadReach);
                    frame(local_only,Light::point); frame(local_only,Light::point);
                    signal=renderer.capture_rt_shadow_signal();
                    std::fprintf(stderr,"rt-shadows: roof beyond the reach: %.3f\n",signal_at(signal,under,2));
                    CHECK(std::abs(signal_at(signal,under,2)-1.0)<=0.01);
                    roof_y=0.2f;
                    vertical_plate=true; roof=false;
                    renderer.set_rt_trace_lamps(true);
                }
                // (b5) a moving camera must not drag the shadows: after a dolly (the projection scale grows every frame) the denoised signal equals the one of a camera that
                // stood still at the final scale (the history is reprojected by the motion vectors)
                {
                    const float final_zoom=1.25f;
                    for(int i=0;i<40;++i) frame(local_only,Light::point,true,final_zoom);
                    const auto still=renderer.capture_rt_shadow_filtered();
                    for(int i=0;i<30;++i) frame(local_only,Light::point,true,1.f+(final_zoom-1.f)*float(i)/29.f);
                    const auto moved=renderer.capture_rt_shadow_filtered();
                    CHECK(still.width==moved.width && still.height==moved.height);
                    double sum=0, worst=0; size_t differing=0;
                    for(size_t i=0;i<still.factor.size();++i) { const double d=std::abs(double(still.factor[i])-double(moved.factor[i])); sum+=d; worst=std::max(worst,d); differing+=d>0.1; }
                    std::fprintf(stderr,"rt-shadows: dolly vs still: mean %.4f, worst %.3f, %.2f%% of the pixels differ by more than 0.1\n",sum/double(still.factor.size()),worst,100.0*double(differing)/double(still.factor.size()));
                    CHECK(sum/double(still.factor.size())<0.005 && double(differing)/double(still.factor.size())<0.01);
                }
                // (b6) ray-traced ambient occlusion alone (plan p4): the floor at distance d from a wall is occluded by the share of cosine-weighted rays that reach the wall
                // within the radius (a CPU estimate with the same direction function); far from the wall nothing changes; GTAO does not run while it is on
                {
                    renderer.set_rt_trace_lamps(false); vertical_plate=false; wall=true;
                    const auto preset=uc::rt_ao_preset(uc::EffectLevel::ultra);
                    const auto occluded_share=[&](double d) {   // floor point at distance d from the wall plane z = -2.8: a ray of direction v reaches it when v.z < 0 and d / -v.z <= radius
                        int hit=0, total=0;
                        for(int i=0;i<128;++i) for(int j=0;j<128;++j) {
                            const auto v=uc::rt_ao_direction({0,1,0},(float(i)+0.5f)/128.f,(float(j)+0.5f)/128.f);
                            const double t=v[2]<-1e-6f ? d/-double(v[2]) : 1e9;
                            if(t<=preset.radius && -1.0+t*double(v[1])<=3.0) ++hit;   // within the radius and under the wall's top edge (y = 3)
                            ++total;
                        }
                        return double(hit)/total;
                    };
                    const Setting ao_only{true,uc::EffectLevel::off,uc::EffectLevel::off,uc::EffectLevel::off,uc::EffectLevel::ultra};
                    const auto occlusion_runs=renderer.pass_runs(uc::GpuPass::occlusion);
                    Setting both=ao_only; both.gtao=uc::EffectLevel::high;
                    for(int i=0;i<48;++i) frame(both,Light::point);
                    CHECK(renderer.pass_runs(uc::GpuPass::occlusion)==occlusion_runs);   // replaced: GTAO does not run while RT AO is on
                    const auto filtered=renderer.capture_rt_shadow_filtered();
                    const auto filtered_at=[&](const Vec& p) { const auto at=pixel_of(p); return double(filtered.factor[size_t(at[1]/1)*filtered.width+at[0]]); };
                    CHECK(filtered.width==1280 && filtered.height==720);   // ultra traces at full resolution
                    std::fprintf(stderr,"rt-shadows: ambient occlusion signal: minimum %.3f, runs of the visibility pass %llu\n",double(*std::min_element(filtered.factor.begin(),filtered.factor.end())),(unsigned long long)renderer.pass_runs(uc::GpuPass::rt_shadows));
                    for(const double d:{0.15,0.35,0.6}) {
                        const Vec p{0.0,-1.0,-2.8+d};
                        const double expected_factor=uc::rt_ao_factor(float(occluded_share(d)),preset.strength), got=filtered_at(p);
                        std::fprintf(stderr,"rt-shadows: ambient occlusion at %.2f from the wall: %.3f (expected %.3f)\n",d,got,expected_factor);
                        CHECK(std::abs(got-expected_factor)<=0.12);
                    }
                    wall=false;   // the same floor point without the wall: open floor, nothing occluded
                    for(int i=0;i<200;++i) frame(ao_only,Light::point);   // the denoiser's history (up to 32 frames) forgets the wall
                    const auto open=renderer.capture_rt_shadow_filtered();
                    const auto open_at=[&](const Vec& p) { const auto at=pixel_of(p); return double(open.factor[size_t(at[1])*open.width+at[0]]); };
                    std::fprintf(stderr,"rt-shadows: ambient occlusion on open floor: %.3f (z -2.65), %.3f (z -2.2), %.3f (z -4), minimum %.3f\n",open_at({0.0,-1.0,-2.65}),open_at({0.0,-1.0,-2.2}),open_at({0.0,-1.0,-4.0}),double(*std::min_element(open.factor.begin(),open.factor.end())));
                    CHECK(std::abs(open_at({0.0,-1.0,-2.65})-1.0)<=0.03);
                    // an unlit surface (baked or emissive) keeps its colour: no occlusion on it (a lamp's glass must not be darkened by its own fixture)
                    {
                        wall=true;
                        const auto grey_at=[&](const uc::RenderPixels& pixels,const Vec& p) { const auto at=pixel_of(p); return int(pixels.rgba[(size_t(at[1])*pixels.width+at[0])*4+1]); };
                        const Setting none{false,uc::EffectLevel::off,uc::EffectLevel::off,uc::EffectLevel::off};
                        frame(none,Light::point,false); const auto before=frame(none,Light::point,false);
                        for(int i=0;i<40;++i) frame(ao_only,Light::point,false);
                        const auto after=frame(ao_only,Light::point,false);
                        std::fprintf(stderr,"rt-shadows: unlit floor beside the wall: off %d, ambient occlusion %d\n",grey_at(before,{0.0,-1.0,-2.65}),grey_at(after,{0.0,-1.0,-2.65}));
                        CHECK(std::abs(grey_at(before,{0.0,-1.0,-2.65})-grey_at(after,{0.0,-1.0,-2.65}))<=1);
                        wall=false;
                    }
                    Setting gtao_only{false,uc::EffectLevel::off,uc::EffectLevel::off,uc::EffectLevel::off,uc::EffectLevel::off,uc::EffectLevel::high};
                    for(int i=0;i<3;++i) frame(gtao_only,Light::point);
                    CHECK(renderer.pass_runs(uc::GpuPass::occlusion)>occlusion_runs);   // without RT AO the raster one runs again
                    vertical_plate=true; wall=false; renderer.set_rt_trace_lamps(true);
                }
                // (c) nothing is lit: every factor is 1 (Review Focus 1)
                frame(sun_only,Light::directional,false); frame(sun_only,Light::directional,false);
                signal=renderer.capture_rt_shadow_signal();
                CHECK(*std::min_element(signal.factor.begin(),signal.factor.end())>=0.999f);
                // (d) off: nothing runs and nothing is allocated
                frame(off,Light::directional); frame(off,Light::directional);
                const uint64_t runs=renderer.pass_runs(uc::GpuPass::rt_shadows); const uint32_t images=renderer.rt_shadow_signal_images();
                frame(off,Light::directional); frame(off,Light::directional);
                CHECK(renderer.pass_runs(uc::GpuPass::rt_shadows)==runs && renderer.rt_shadow_signal_images()==images);
                // (e) frames: the shadowed floor is darker with the effect, the far floor is not; the raster shadow map does not run while RT shadows replace it
                const Setting both_raster{true,uc::EffectLevel::medium,uc::EffectLevel::off,uc::EffectLevel::medium};
                frame(off,Light::directional); const auto picture_off=frame(off,Light::directional);
                const auto shadow_runs=renderer.pass_runs(uc::GpuPass::shadow);
                for(int i=0;i<3;++i) frame(both_raster,Light::directional);
                const auto picture_on=frame(both_raster,Light::directional);
                CHECK(renderer.pass_runs(uc::GpuPass::shadow)==shadow_runs);   // replaced: no shadow map while RT shadows are active (the raster level stays medium)
                const auto grey=[&](const uc::RenderPixels& pixels,const Vec& p) { const auto at=pixel_of(p); return int(pixels.rgba[(size_t(at[1])*pixels.width+at[0])*4+1]); };
                std::fprintf(stderr,"rt-shadows: picture shadowed floor off %d on %d, far floor off %d on %d\n",grey(picture_off,shadowed_sun),grey(picture_on,shadowed_sun),grey(picture_off,lit_sun),grey(picture_on,lit_sun));
                CHECK(grey(picture_on,shadowed_sun)<=grey(picture_off,shadowed_sun)-25 && std::abs(grey(picture_on,lit_sun)-grey(picture_off,lit_sun))<=1);
                // (f) the master switch off with the levels kept: the raster shadow map runs again, no RT pass
                const auto rt_runs=renderer.pass_runs(uc::GpuPass::rt_shadows);
                const Setting master_off{false,uc::EffectLevel::medium,uc::EffectLevel::off,uc::EffectLevel::medium};
                for(int i=0;i<3;++i) frame(master_off,Light::directional);
                CHECK(renderer.pass_runs(uc::GpuPass::rt_shadows)==rt_runs && renderer.pass_runs(uc::GpuPass::shadow)>shadow_runs);
                // (g) toggling between presets that share a resolution allocates nothing new; a still camera does not shimmer with soft shadows
                const Setting soft{true,uc::EffectLevel::high,uc::EffectLevel::off,uc::EffectLevel::off};
                for(int i=0;i<8;++i) frame(i%2 ? sun_only : soft,Light::directional);
                const auto growths=renderer.rt_buffer_growths(); const auto signal_images=renderer.rt_shadow_signal_images();
                for(int i=0;i<20;++i) frame(i%2 ? sun_only : soft,Light::directional);
                CHECK(renderer.rt_buffer_growths()==growths && renderer.rt_shadow_signal_images()==signal_images);
                std::vector<int> series;
                for(int i=0;i<20;++i) { const auto picture=frame(soft,Light::directional); if(i>=12) series.push_back(grey(picture,shadowed_sun)); }
                double mean=0; for(int v:series) mean+=v; mean/=double(series.size());
                double deviation=0; for(int v:series) deviation+=(v-mean)*(v-mean);
                deviation=std::sqrt(deviation/double(series.size()));
                std::fprintf(stderr,"rt-shadows: soft shadow over 8 still frames: mean %.1f deviation %.2f\n",mean,deviation);
                CHECK(deviation<2.0);
                frame(off,Light::directional); frame(off,Light::directional);
            }
        }
        if (wanted("rt-cache")) {
            // The cached object-space BLASes of static meshes (rt_mesh_cache.h, plan p3 Task 17): a floor and one plate mesh drawn twice (the second time mirrored and moved
            // by its matrix) under a point lamp. The BLAS of a mesh is built once whatever its matrix; the shadows follow the instances' transforms.
            if (!renderer.ray_tracing_supported()) std::fprintf(stderr,"rt-cache: skipped, the device has no ray tracing\n");
            else {
                CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
                renderer.set_rt_trace_lamps(true);
                using Vec=std::array<double,3>;
                const gx::Projection projection{{{1,0,0,0},{0,1,0,0},{0,0,-100.f/99,-100.f/99},{0,0,-1,0}}};
                const std::array<float,4> material{0.8f,0.6f,0.4f,1.f}, ambient{0.35f,0.35f,0.35f,1.f};
                const std::array<std::array<float,3>,4> floor_corners{{{-12,-1,-2},{12,-1,-2},{12,-1,-8},{-12,-1,-8}}}, plate_corners{{{-0.5f,-1,-4},{0.5f,-1,-4},{0.5f,-0.4f,-4},{-0.5f,-0.4f,-4}}};
                const std::array<float,4> lamp_position{1.5f,1.5f,-3.f,0};
                const auto make_mesh=[&](const std::array<std::array<float,3>,4>& corners) {
                    auto draw=quad(0x1000,-1,-1,1,1,{180,180,180,255},true);
                    for(size_t i=0;i<4;++i) { draw.vertices[i].position=corners[i]; draw.vertices[i].normal={0,1,0,0,0,0,0,0,0}; }
                    return std::make_shared<const gx::MeshBuffers>(gx::MeshBuffers{draw.vertices,draw.indices});
                };
                auto floor_mesh=make_mesh(floor_corners), plate_mesh=make_mesh(plate_corners);
                const std::array<std::array<float,3>,4> roof_corners{{{-1,0.2f,-5},{1,0.2f,-5},{1,0.2f,-3},{-1,0.2f,-3}}};
                auto roof_mesh=make_mesh(roof_corners); bool roof_on=false;   // a horizontal plate above the floor: the key light's shadow falls beside it
                float camera_z=0.f;   // every draw's view-space z offset: a camera moving along its axis (the meshes stay the same objects, so the motion tracker pairs the draws)
                uc::EffectLevel local_level=uc::EffectLevel::medium;
                const auto surface=[&](const std::shared_ptr<const gx::MeshBuffers>& mesh,bool plate,std::array<float,4> row0) {   // row0: the x row of the object-to-view matrix
                    auto draw=quad(0x1000,-1,-1,1,1,{180,180,180,255},true);
                    draw.transforms.edit().projection=projection; draw.transforms.edit().position_rows[0]=row0; draw.transforms.edit().position_rows[2]={0,0,1,camera_z};
                    auto& uniforms=draw.uniforms.edit();
                    uniforms.pixel[1]=1u|(3u<<1)|(1u<<4);
                    uniforms.channels[0]=2|(1<<2)|(2<<7)|(3<<9);
                    uniforms.materials[0]=material; uniforms.ambient[0]=ambient;
                    uniforms.lights[0].color={255,255,255,255}; uniforms.lights[0].direction={0,0,1,0}; uniforms.lights[0].position=lamp_position;
                    uniforms.lights[0].distance={1,0,0,0}; uniforms.lights[0].cosine={1,0,0,0};
                    if(plate) { draw.material.stages=1; draw.material.swaps={0xE4,0xE4,0xE4,0xE4}; draw.material.stage[0]={0x8F8AF,0x8FFC0,64,0,0}; }
                    draw.mesh=mesh; draw.vertices.clear(); draw.indices.clear();
                    return draw;
                };
                const std::array<float,4> mirrored{-1,0,0,-3};   // x' = -x - 3: the plate mirrored to x in [-3.5, -2.5]
                auto frame=[&](float shift,bool fresh_meshes,bool rt=true) {
                    uc::GraphicsEffects effects; effects.anisotropy=1; effects.ray_tracing=rt; effects.rt_local_shadows=local_level; renderer.set_effects(effects);
                    if(fresh_meshes) { floor_mesh=make_mesh(floor_corners); plate_mesh=make_mesh(plate_corners); }   // a new identity every frame: the cache must not grow without bound
                    gx::FramePacket scene; scene.present=false;
                    scene.draws.push_back(surface(floor_mesh,false,{1,0,0,0}));
                    scene.draws.push_back(surface(plate_mesh,true,{1,0,0,shift}));
                    scene.draws.push_back(surface(plate_mesh,true,mirrored));
                    if(roof_on) scene.draws.push_back(surface(roof_mesh,true,{1,0,0,0}));
                    submit(std::move(scene));
                    gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                    auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                    boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                    submit(std::move(boundary));
                    gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                    submit(std::move(empty));
                    gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                    hud.draws.push_back(quad(0x5678,0.6f,-0.9f,0.9f,-0.6f,{255,0,0,255},false));
                    submit(std::move(hud));
                };
                // The CPU reference: the lamp's ray from a floor point crosses the plane z = -4 at (x, y); a plate blocks it when x is in its range and y in [-1, -0.4].
                const auto crossing=[&](const Vec& p) { const double t=(-4-p[2])/(lamp_position[2]-p[2]); return std::array<double,3>{p[0]+(lamp_position[0]-p[0])*t,p[1]+(lamp_position[1]-p[1])*t,t}; };
                const auto blocked_by=[&](const Vec& p,double x0,double x1) { const auto c=crossing(p); return c[2]>0 && c[2]<1 && c[0]>=x0 && c[0]<=x1 && c[1]>=-1 && c[1]<=-0.4; };
                const auto pixel_of=[&](const Vec& p) { return std::array<uint32_t,2>{uint32_t((p[0]/-p[2]+1)/2*1280),uint32_t((p[1]/-p[2]+1)/2*720)}; };
                const auto hidden_behind=[&](const Vec& p,double x0,double x1) { const auto at=pixel_of(p); return at[0]>=uint32_t((x0/4+1)/2*1280)-15 && at[0]<=uint32_t((x1/4+1)/2*1280)+15 && at[1]>=260 && at[1]<=335; };
                const auto expected=[&](const Vec& p,bool blocked) {
                    gx::LightUniform lamp; lamp.color={255,255,255,255}; lamp.direction={0,0,1,0}; lamp.position=lamp_position; lamp.distance={1,0,0,0}; lamp.cosine={1,0,0,0};
                    const float contribution=uc::gx_light_contribution(lamp,3,2,{float(p[0]),float(p[1]),float(p[2])},{0,1,0});
                    const float visibility[]={blocked ? 0.f : 1.f};
                    return uc::shadow_factor(ambient[0],std::array<float,1>{contribution},visibility);
                };
                // A floor point the camera sees whose lamp ray is blocked by exactly the given plate (the widest margin the grid offers) and by no other.
                const auto pick=[&](double plate_x0,double plate_x1,double other_x0,double other_x1,double hide_a0,double hide_a1,double hide_b0,double hide_b1) {
                    for(const double margin:{0.2,0.12,0.08}) {
                        std::vector<Vec> found;
                        for(double x=-6;x<=6;x+=0.05) for(double z=-5.5;z<=-4.2;z+=0.05) {
                            const Vec p{x,-1,z}; bool good=!hidden_behind(p,hide_a0,hide_a1) && !hidden_behind(p,hide_b0,hide_b1);
                            for(const double dx:{-margin,0.0,margin}) for(const double dz:{-margin,0.0,margin}) { const Vec q{p[0]+dx,p[1],p[2]+dz}; good=good && blocked_by(q,plate_x0,plate_x1) && !blocked_by(q,other_x0,other_x1); }
                            if(good) found.push_back(p);
                        }
                        if(!found.empty()) return found[found.size()/2];
                    }
                    throw std::runtime_error("rt-cache: no sample point");
                };
                const auto signal_at=[&](const uc::VulkanRenderer::RtShadowPixels& signal,const Vec& p) { const auto at=pixel_of(p); return double(signal.factor[size_t(at[1]/2)*signal.width+(at[0]/2)]); };
                // (a) two instances of one mesh, one mirrored: one BLAS built for the mesh, both shadows right
                const auto base=renderer.rt_cache_stats();   // other sections of this process may have cached meshes already: count what this one adds
                frame(0,false); frame(0,false);
                auto signal=renderer.capture_rt_shadow_signal();
                const Vec behind_plate_one=pick(-0.5,0.5,-3.5,-2.5,-0.5,0.5,-3.5,-2.5), behind_mirrored=pick(-3.5,-2.5,-0.5,0.5,-0.5,0.5,-3.5,-2.5);
                const double one=signal_at(signal,behind_plate_one), two=signal_at(signal,behind_mirrored);
                std::fprintf(stderr,"rt-cache: shadow of the plate %.3f (expected %.3f), of its mirrored instance %.3f (expected %.3f)\n",one,expected(behind_plate_one,true),two,expected(behind_mirrored,true));
                CHECK(std::abs(one-expected(behind_plate_one,true))<=0.05 && std::abs(two-expected(behind_mirrored,true))<=0.05);
                auto cache=renderer.rt_cache_stats();
                std::fprintf(stderr,"rt-cache: builds %llu hits %llu resident %u, static instances %u\n",(unsigned long long)cache.builds,(unsigned long long)cache.hits,cache.resident,renderer.rt_stats().static_instances);
                CHECK(cache.builds-base.builds==2 && cache.hits>base.hits && renderer.rt_stats().static_instances==3 && renderer.rt_stats().skinned_blas==0);   // the floor mesh and the plate mesh (one BLAS for both plate instances); three instances
                // (b) a new matrix, no new BLAS: the plate moves to x in [-1.5, -0.5] (its shadow then reaches past its own silhouette, where the camera sees it)
                const uint64_t builds=cache.builds;
                frame(-1.0f,false); frame(-1.0f,false);
                signal=renderer.capture_rt_shadow_signal();
                const Vec behind_moved=pick(-1.5,-0.5,-3.5,-2.5,-1.5,-0.5,-3.5,-2.5);
                CHECK(renderer.rt_cache_stats().builds==builds);
                const Vec old_shadow=pick(-0.5,0.5,-1.5,-0.5,-1.5,-0.5,-3.5,-2.5);   // in the old plate's shadow, outside the moved plate's; only the moved plate and the mirrored one hide floor
                CHECK(std::abs(signal_at(signal,behind_moved)-expected(behind_moved,true))<=0.05 && std::abs(signal_at(signal,old_shadow)-1.0)<=0.01);   // the old shadow is gone, the new one is there
                // (c) a new mesh identity every frame (the guest frees and re-creates meshes): the swept meshes' BLASes are dropped, nothing grows without bound
                for(int i=0;i<120;++i) frame(0,true);
                cache=renderer.rt_cache_stats();
                std::fprintf(stderr,"rt-cache: after 120 frames with fresh meshes: resident %u, builds %llu, invalidated %llu\n",cache.resident,(unsigned long long)cache.builds,(unsigned long long)cache.invalidated);
                CHECK(cache.invalidated>0 && cache.resident<200);
                // (d) off again: the cache is not consulted
                const uint64_t after=renderer.rt_cache_stats().builds;
                frame(0,false,false); frame(0,false,false);
                CHECK(renderer.rt_cache_stats().builds==after);
                // (e) a camera moving along its axis must not wear the denoiser's history out: with the soft key light the denoised signal after a 3-unit approach at 0.1 unit per
                // frame (2-3% of the distance: more than the history's depth tolerance) is as close to the converged still-camera signal as a still camera's own later frame is
                {
                    renderer.set_rt_trace_lamps(false); local_level=uc::EffectLevel::high; roof_on=true;
                    for(int i=0;i<80;++i) frame(0,false);
                    const auto converged=renderer.capture_rt_shadow_filtered();
                    for(int i=0;i<6;++i) frame(0,false);
                    const auto later=renderer.capture_rt_shadow_filtered();
                    for(int i=0;i<=30;++i) { camera_z=std::max(0.f,3.f-0.1f*float(i)); frame(0,false); }
                    const auto approached=renderer.capture_rt_shadow_filtered();
                    camera_z=0.f; local_level=uc::EffectLevel::medium; roof_on=false; renderer.set_rt_trace_lamps(true);
                    CHECK(converged.factor.size()==approached.factor.size());
                    double still_error=0, moving_error=0;
                    for(size_t i=0;i<converged.factor.size();++i) { still_error+=std::abs(double(later.factor[i])-double(converged.factor[i])); moving_error+=std::abs(double(approached.factor[i])-double(converged.factor[i])); }
                    still_error/=double(converged.factor.size()); moving_error/=double(converged.factor.size());
                    size_t shadowed=0; for(const float v:converged.factor) shadowed+=v<0.9f;
                    std::fprintf(stderr,"rt-cache: denoiser after an approach: error %.5f against %.5f of a still camera, %.2f%% of the signal shadowed\n",moving_error,still_error,100.0*double(shadowed)/double(converged.factor.size()));
                    CHECK(shadowed>converged.factor.size()/200);   // the fixture has a shadow to smear
                    CHECK(moving_error<=still_error*2+0.002);
                }
            }
        }
        if (wanted("rt-reflections")) {
            // Ray-traced reflections (rt_reflection_pass.h, plan p5): a glossy floor (channel 1 lit with the specular function, specular material white) under a red plate
            // that stands ahead of the camera. The mirror ray of the floor point (0, -1, -3) reaches the plate at (0, 0, -6); the frame shows the plate there, so the floor takes
            // fresnel x reflectivity of the plate's colour. A matte floor, a hidden hit and the effect off leave the floor as it is.
            if (!renderer.ray_tracing_supported()) std::fprintf(stderr,"rt-reflections: skipped, the device has no ray tracing\n");
            else {
                CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
                using Vec=std::array<float,3>;
                const gx::Projection projection{{{1,0,0,0},{0,1,0,0},{0,0,-100.f/99,-100.f/99},{0,0,-1,0}}};
                const auto make_mesh=[&](const std::array<Vec,4>& corners,std::array<uint8_t,4> color,Vec normal={0,0,1}) {
                    auto draw=quad(0x1000,-1,-1,1,1,color,true);
                    for(size_t i=0;i<4;++i) { draw.vertices[i].position=corners[i]; draw.vertices[i].normal={normal[0],normal[1],normal[2],0,0,0,0,0,0}; draw.vertices[i].color[0]=color; }
                    return std::make_shared<const gx::MeshBuffers>(gx::MeshBuffers{draw.vertices,draw.indices});
                };
                const auto floor_mesh=make_mesh({{{-12,-1,-2},{12,-1,-2},{12,-1,-8},{-12,-1,-8}}},{180,180,180,255},{0,1,0});
                const auto plate_mesh=make_mesh({{{-3,-0.5f,-6},{3,-0.5f,-6},{3,2,-6},{-3,2,-6}}},{255,0,0,255});
                const auto cover_mesh=make_mesh({{{-0.3f,-0.2f,-4.5f},{0.3f,-0.2f,-4.5f},{0.3f,0.2f,-4.5f},{-0.3f,0.2f,-4.5f}}},{0,0,255,255});   // a nearer blue quad hiding the plate's pixel at the hit
                const auto draw_of=[&](const std::shared_ptr<const gx::MeshBuffers>& mesh,bool floor,float specular) {
                    auto draw=quad(0x1000,-1,-1,1,1,{180,180,180,255},true);
                    draw.transforms.edit().projection=projection;
                    auto& uniforms=draw.uniforms.edit();
                    uniforms.pixel[1]=1u|(3u<<1)|(1u<<4);
                    if(floor) {
                        uniforms.channels[0]=2|(1<<2)|(2<<7)|(3<<9); uniforms.channels[1]=2|(1<<9);   // channel 1: lit with the specular function
                        uniforms.materials[0]={0.8f,0.6f,0.4f,1.f}; uniforms.ambient[0]={0.5f,0.5f,0.5f,1.f}; uniforms.materials[1]={specular,specular,specular,1.f};
                        uniforms.lights[0].color={120,120,120,255}; uniforms.lights[0].position={0,5,0,0}; uniforms.lights[0].distance={1,0,0,0}; uniforms.lights[0].cosine={1,0,0,0};
                    }
                    draw.mesh=mesh; draw.vertices.clear(); draw.indices.clear();
                    return draw;
                };
                bool plate_on=true;
                auto frame=[&](bool rt,uc::EffectLevel level,float specular,bool cover) {
                    uc::GraphicsEffects effects; effects.anisotropy=1; effects.ray_tracing=rt; effects.rt_reflections=level; renderer.set_effects(effects);
                    gx::FramePacket scene; scene.present=false;
                    scene.draws.push_back(draw_of(floor_mesh,true,specular));
                    if(plate_on) scene.draws.push_back(draw_of(plate_mesh,false,0.f));
                    if(cover) scene.draws.push_back(draw_of(cover_mesh,false,0.f));
                    submit(std::move(scene));
                    gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                    auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                    boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                    submit(std::move(boundary));
                    gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                    submit(std::move(empty));
                    gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                    hud.draws.push_back(quad(0x5678,0.6f,-0.9f,0.9f,-0.6f,{255,0,0,255},false));
                    return renderer.capture(submit(std::move(hud)));
                };
                const auto at=[&](const uc::RenderPixels& pixels,int x,int y,int channel) { return int(pixels.rgba[(size_t(y)*pixels.width+size_t(x))*4+size_t(channel)]); };
                const int floor_x=640, floor_y=240, plate_y=360;   // pixels of (0, -1, -3) and (0, 0, -6)
                const auto settle=[&](bool rt,uc::EffectLevel level,float specular,bool cover) { for(int i=0;i<40;++i) frame(rt,level,specular,cover); return frame(rt,level,specular,cover); };
                const auto off=settle(false,uc::EffectLevel::off,1.f,false);
                const auto on=settle(true,uc::EffectLevel::medium,1.f,false);
                const float weight=std::min(uc::rt_fresnel(1.f/std::sqrt(10.f))*uc::rt_reflectivity({1,1,1},true)*uc::kRtReflectStrength,1.f);
                std::fprintf(stderr,"rt-reflections: floor off (%d %d %d), on (%d %d %d), plate (%d %d %d), weight %.3f\n",at(off,floor_x,floor_y,0),at(off,floor_x,floor_y,1),at(off,floor_x,floor_y,2),at(on,floor_x,floor_y,0),at(on,floor_x,floor_y,1),at(on,floor_x,floor_y,2),
                    at(on,floor_x,plate_y,0),at(on,floor_x,plate_y,1),at(on,floor_x,plate_y,2),double(weight));
                const std::array<float,3> plate{float(at(on,floor_x,plate_y,0)),float(at(on,floor_x,plate_y,1)),float(at(on,floor_x,plate_y,2))};
                CHECK(plate[0]>200 && plate[1]<40);   // the plate is red where the ray hits it, and the effect does not touch it
                for(int channel=0;channel<3;++channel) {
                    const float expected=float(at(off,floor_x,floor_y,channel))*(1.f-weight)+plate[size_t(channel)]*weight;
                    CHECK(std::abs(float(at(on,floor_x,floor_y,channel))-expected)<=6.f);
                }
                CHECK(at(on,floor_x,floor_y,0)>at(off,floor_x,floor_y,0)+3);   // the effect is visible: more red than without
                const auto matte=settle(true,uc::EffectLevel::medium,0.f,false);
                CHECK(std::abs(at(matte,floor_x,floor_y,0)-at(off,floor_x,floor_y,0))<=1 && std::abs(at(matte,floor_x,floor_y,2)-at(off,floor_x,floor_y,2))<=1);   // no gloss, no reflection
                const auto hidden_off=settle(false,uc::EffectLevel::off,1.f,true), hidden_on=settle(true,uc::EffectLevel::medium,1.f,true);
                std::fprintf(stderr,"rt-reflections: hit hidden by a nearer quad: floor off %d on %d\n",at(hidden_off,floor_x,floor_y,0),at(hidden_on,floor_x,floor_y,0));
                CHECK(std::abs(at(hidden_on,floor_x,floor_y,0)-at(hidden_off,floor_x,floor_y,0))<=1);
                // a reflection follows what it reflects: after the plate is gone the floor is back to its own colour within a few frames (a long history would keep the plate's trail)
                settle(true,uc::EffectLevel::medium,1.f,false);
                plate_on=false;
                for(int i=0;i<20;++i) frame(true,uc::EffectLevel::medium,1.f,false);
                const auto trail=frame(true,uc::EffectLevel::medium,1.f,false);
                const auto bare=settle(false,uc::EffectLevel::off,1.f,false);
                std::fprintf(stderr,"rt-reflections: 20 frames after the plate is gone: floor red %d (bare %d)\n",at(trail,floor_x,floor_y,0),at(bare,floor_x,floor_y,0));
                CHECK(std::abs(at(trail,floor_x,floor_y,0)-at(bare,floor_x,floor_y,0))<=3);
                plate_on=true;
                const uint64_t runs=renderer.pass_runs(uc::GpuPass::rt_reflections);
                frame(false,uc::EffectLevel::off,1.f,false); frame(false,uc::EffectLevel::off,1.f,false);
                CHECK(renderer.pass_runs(uc::GpuPass::rt_reflections)==runs);   // off: nothing runs
                const auto unmastered=settle(false,uc::EffectLevel::medium,1.f,false);
                CHECK(std::abs(at(unmastered,floor_x,floor_y,0)-at(off,floor_x,floor_y,0))<=1);   // the master switch off with the level kept: nothing runs
            }
        }
        if (wanted("rt-lighting")) {
            // Ray-traced lighting (plan p6): a floor in front of a bright red wall takes a share of the wall's colour, by the CPU estimate of the cosine-weighted rays that reach the
            // wall; the far floor does not; the effect off and the master off change nothing.
            if (!renderer.ray_tracing_supported()) std::fprintf(stderr,"rt-lighting: skipped, the device has no ray tracing\n");
            else {
                CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
                using Vec=std::array<float,3>;
                const gx::Projection projection{{{1,0,0,0},{0,1,0,0},{0,0,-100.f/99,-100.f/99},{0,0,-1,0}}};
                const auto make_mesh=[&](const std::array<Vec,4>& corners,std::array<uint8_t,4> color,Vec normal) {
                    auto draw=quad(0x1000,-1,-1,1,1,color,true);
                    for(size_t i=0;i<4;++i) { draw.vertices[i].position=corners[i]; draw.vertices[i].normal={normal[0],normal[1],normal[2],0,0,0,0,0,0}; draw.vertices[i].color[0]=color; }
                    return std::make_shared<const gx::MeshBuffers>(gx::MeshBuffers{draw.vertices,draw.indices});
                };
                const auto floor_mesh=make_mesh({{{-12,-1,-2},{12,-1,-2},{12,-1,-8},{-12,-1,-8}}},{180,180,180,255},{0,1,0});
                const auto wall_mesh=make_mesh({{{-12,-1,-5},{12,-1,-5},{12,3,-5},{-12,3,-5}}},{255,0,0,255},{0,0,1});
                const auto draw_of=[&](const std::shared_ptr<const gx::MeshBuffers>& mesh,bool floor) {
                    auto draw=quad(0x1000,-1,-1,1,1,{180,180,180,255},true);
                    draw.transforms.edit().projection=projection;
                    auto& uniforms=draw.uniforms.edit();
                    uniforms.pixel[1]=1u|(3u<<1)|(1u<<4);
                    if(floor) {
                        uniforms.channels[0]=2|(1<<2)|(2<<7)|(3<<9);
                        uniforms.materials[0]={0.8f,0.6f,0.4f,1.f}; uniforms.ambient[0]={0.3f,0.3f,0.3f,1.f};
                        uniforms.lights[0].color={80,80,80,255}; uniforms.lights[0].position={0,5,0,0}; uniforms.lights[0].distance={1,0,0,0}; uniforms.lights[0].cosine={1,0,0,0};
                    }
                    draw.mesh=mesh; draw.vertices.clear(); draw.indices.clear();
                    return draw;
                };
                auto frame=[&](bool rt,uc::EffectLevel level) {
                    uc::GraphicsEffects effects; effects.anisotropy=1; effects.ray_tracing=rt; effects.rt_lighting=level; renderer.set_effects(effects);
                    gx::FramePacket scene; scene.present=false;
                    scene.draws.push_back(draw_of(floor_mesh,true)); scene.draws.push_back(draw_of(wall_mesh,false));
                    submit(std::move(scene));
                    gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                    auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                    boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                    submit(std::move(boundary));
                    gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                    submit(std::move(empty));
                    gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                    hud.draws.push_back(quad(0x5678,0.6f,-0.9f,0.9f,-0.6f,{255,0,0,255},false));
                    return renderer.capture(submit(std::move(hud)));
                };
                const auto at=[&](const uc::RenderPixels& pixels,int x,int y,int channel) { return int(pixels.rgba[(size_t(y)*pixels.width+size_t(x))*4+size_t(channel)]); };
                const auto settle=[&](bool rt,uc::EffectLevel level) { for(int i=0;i<40;++i) frame(rt,level); return frame(rt,level); };
                const auto pixel_row=[&](float z) { return int((-1.f/-z+1.f)/2.f*720.f); };   // the row of the floor point (0, -1, z)
                const uc::EffectLevel level=uc::EffectLevel::medium;
                const auto preset=uc::rt_light_preset(level);
                // the CPU estimate: the share of cosine-weighted rays from (0, -1, -4.5) that reach the wall at z = -5 within the reach (the wall's top edge is far above)
                const auto bounce=[&](float distance) {
                    double sum=0; int total=0;
                    for(int i=0;i<128;++i) for(int j=0;j<128;++j) {
                        const auto v=uc::rt_ao_direction({0,1,0},(float(i)+0.5f)/128.f,(float(j)+0.5f)/128.f);
                        const double t=v[2]<-1e-6f ? distance/-double(v[2]) : 1e9;
                        if(t<=preset.reach) sum+=uc::rt_hit_confidence(float(t),preset.reach,1.f);
                        ++total;
                    }
                    return sum/total;
                };
                const int x=640, near_row=pixel_row(-4.5f), far_row=pixel_row(-2.6f);
                const auto off=settle(false,uc::EffectLevel::off), on=settle(true,level);
                const double irradiance=bounce(0.5f), expected=double(at(off,x,near_row,0))+0.8*irradiance*preset.strength*255.0;
                std::fprintf(stderr,"rt-lighting: floor beside the wall: red off %d on %d (expected %.1f), green off %d on %d; far floor red off %d on %d\n",at(off,x,near_row,0),at(on,x,near_row,0),expected,at(off,x,near_row,1),at(on,x,near_row,1),at(off,x,far_row,0),at(on,x,far_row,0));
                CHECK(at(on,x,near_row,0)>at(off,x,near_row,0)+5 && std::abs(double(at(on,x,near_row,0))-expected)<=10.0);   // it adds red, by the estimate
                CHECK(std::abs(at(on,x,near_row,1)-at(off,x,near_row,1))<=2 && std::abs(at(on,x,near_row,2)-at(off,x,near_row,2))<=2);   // and only red: the wall is red
                const double expected_far=double(at(off,x,far_row,0))+0.8*bounce(2.4f)*preset.strength*255.0;   // 2.4 units from the wall: less light than at 0.5
                CHECK(std::abs(double(at(on,x,far_row,0))-expected_far)<=10.0 && at(on,x,far_row,0)-at(off,x,far_row,0)<at(on,x,near_row,0)-at(off,x,near_row,0));
                const uint64_t runs=renderer.pass_runs(uc::GpuPass::rt_lighting);
                frame(false,uc::EffectLevel::off); frame(false,uc::EffectLevel::off);
                CHECK(renderer.pass_runs(uc::GpuPass::rt_lighting)==runs);   // off: nothing runs
                const auto unmastered=settle(false,level);
                CHECK(std::abs(at(unmastered,x,near_row,0)-at(off,x,near_row,0))<=1);   // the master switch off with the level kept
            }
        }
        if (wanted("rt-rr")) {
            // DLSS Ray Reconstruction (plan p7): with DLSS and an RT effect the frame goes through it instead of the own denoiser and Super Resolution. The lighting fixture (a floor in
            // front of a bright red wall) is rendered through RR and through the own denoiser: both add the wall's red to the floor beside it, the picture is not black or broken.
            if (!renderer.ray_tracing_supported() || !renderer.ray_reconstruction_supported()) std::fprintf(stderr,"rt-rr: skipped, no ray tracing or no Ray Reconstruction on this device\n");
            else {
                CHECK(renderer.set_upscaling(uc::UpscalerKind::dlss,uc::UpscaleMode::native,false,0.5,false,2));   // DLAA: the internal size is the output size, the pixels line up
                using Vec=std::array<float,3>;
                const gx::Projection projection{{{1,0,0,0},{0,1,0,0},{0,0,-100.f/99,-100.f/99},{0,0,-1,0}}};
                const auto make_mesh=[&](const std::array<Vec,4>& corners,std::array<uint8_t,4> color,Vec normal) {
                    auto draw=quad(0x1000,-1,-1,1,1,color,true);
                    for(size_t i=0;i<4;++i) { draw.vertices[i].position=corners[i]; draw.vertices[i].normal={normal[0],normal[1],normal[2],0,0,0,0,0,0}; draw.vertices[i].color[0]=color; }
                    return std::make_shared<const gx::MeshBuffers>(gx::MeshBuffers{draw.vertices,draw.indices});
                };
                const auto floor_mesh=make_mesh({{{-12,-1,-2},{12,-1,-2},{12,-1,-8},{-12,-1,-8}}},{180,180,180,255},{0,1,0});
                const auto wall_mesh=make_mesh({{{-12,-1,-5},{12,-1,-5},{12,3,-5},{-12,3,-5}}},{255,0,0,255},{0,0,1});
                const auto draw_of=[&](const std::shared_ptr<const gx::MeshBuffers>& mesh,bool floor) {
                    auto draw=quad(0x1000,-1,-1,1,1,{180,180,180,255},true);
                    draw.transforms.edit().projection=projection;
                    auto& uniforms=draw.uniforms.edit();
                    uniforms.pixel[1]=1u|(3u<<1)|(1u<<4);
                    if(floor) {
                        uniforms.channels[0]=2|(1<<2)|(2<<7)|(3<<9);
                        uniforms.materials[0]={0.8f,0.6f,0.4f,1.f}; uniforms.ambient[0]={0.3f,0.3f,0.3f,1.f};
                        uniforms.lights[0].color={80,80,80,255}; uniforms.lights[0].position={0,5,0,0}; uniforms.lights[0].distance={1,0,0,0}; uniforms.lights[0].cosine={1,0,0,0};
                    }
                    draw.mesh=mesh; draw.vertices.clear(); draw.indices.clear();
                    return draw;
                };
                auto frame=[&](bool rt,bool rr) {
                    uc::GraphicsEffects effects; effects.anisotropy=1; effects.ray_tracing=rt; effects.rt_lighting=rt ? uc::EffectLevel::medium : uc::EffectLevel::off; effects.ray_reconstruction=rr; renderer.set_effects(effects);
                    gx::FramePacket scene; scene.present=false;
                    scene.draws.push_back(draw_of(floor_mesh,true)); scene.draws.push_back(draw_of(wall_mesh,false));
                    submit(std::move(scene));
                    gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
                    auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
                    boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
                    submit(std::move(boundary));
                    gx::FramePacket empty; empty.present=false; empty.clear_after_copy=false;
                    submit(std::move(empty));
                    gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
                    hud.draws.push_back(quad(0x5678,0.6f,-0.9f,0.9f,-0.6f,{255,0,0,255},false));
                    return renderer.capture(submit(std::move(hud)));
                };
                const auto at=[&](const uc::RenderPixels& pixels,int x,int y,int channel) { return int(pixels.rgba[(size_t(y)*pixels.width+size_t(x))*4+size_t(channel)]); };
                const auto settle=[&](bool rt,bool rr) { for(int i=0;i<60;++i) frame(rt,rr); return frame(rt,rr); };
                const int x=640, near_row=int((-1.f/4.5f+1.f)/2.f*720.f);
                const auto off=settle(false,true);
                const auto reconstructed=settle(true,true);   // right after the plain frames: if RR wrote nothing the output would still show them (no bounce)
                CHECK(renderer.ray_reconstruction_active());   // the frame went through Ray Reconstruction
                const auto own=settle(true,false);
                CHECK(!renderer.ray_reconstruction_active());
                std::fprintf(stderr,"rt-rr: floor beside the wall, red: off %d, own denoiser %d, Ray Reconstruction %d; wall red %d, wall green %d\n",at(off,x,near_row,0),at(own,x,near_row,0),at(reconstructed,x,near_row,0),at(reconstructed,x,int((-1.f/5.f+1.f)/2.f*720.f)+40,0),at(reconstructed,x,int((-1.f/5.f+1.f)/2.f*720.f)+40,1));
                CHECK(at(own,x,near_row,0)>at(off,x,near_row,0)+5);
                CHECK(at(reconstructed,x,near_row,0)>at(off,x,near_row,0)+5);   // RR keeps the bounced red
                CHECK(std::abs(at(reconstructed,x,near_row,0)-at(own,x,near_row,0))<=25 && std::abs(at(reconstructed,x,near_row,1)-at(own,x,near_row,1))<=20);   // and lands near the own denoiser's picture
                const int wall_row=int((-1.f/5.f+1.f)/2.f*720.f)+40;
                CHECK(at(reconstructed,x,wall_row,0)>200 && at(reconstructed,x,wall_row,1)<60);   // the wall is still red: the picture is not black or smeared
                CHECK(renderer.set_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::quality,false,0.5,false,2));
                frame(false,true); frame(false,true);
            }
        }
        if (wanted("rt-denoise")) {
            // The own denoiser (rt_denoiser.h) on synthetic signals, outside the frame: two planes facing a still camera (view depth 2 on the left half,
            // 10 on the right), no motion. Noise averages out, a signal edge on the depth edge stays sharp, a reset (camera cut) drops the history.
            constexpr uint32_t width=256, height=128, edge=width/2;
            const VkDevice device=renderer.device;
            const auto& memory=renderer.memory_properties();
            const auto memory_type=[&](uint32_t mask,VkMemoryPropertyFlags flags) {
                for(uint32_t i=0;i<memory.memoryTypeCount;++i) if((mask&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags) return i;
                throw std::runtime_error("rt-denoise: no memory type");
            };
            const auto full_barrier=[](VkCommandBuffer command) {
                VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                barrier.srcStageMask=barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                barrier.srcAccessMask=barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
                VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.memoryBarrierCount=1; info.pMemoryBarriers=&barrier;
                vkCmdPipelineBarrier2(command,&info);
            };
            // Staging memory for uploads and the readback (the largest texel is 16 bytes).
            VkBuffer staging=VK_NULL_HANDLE; VkDeviceMemory staging_memory=VK_NULL_HANDLE; void* mapped=nullptr;
            {
                VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size=VkDeviceSize(width)*height*16; info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                CHECK(vkCreateBuffer(device,&info,nullptr,&staging)==VK_SUCCESS);
                VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device,staging,&requirements);
                VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size;
                allocate.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                CHECK(vkAllocateMemory(device,&allocate,nullptr,&staging_memory)==VK_SUCCESS);
                CHECK(vkBindBufferMemory(device,staging,staging_memory,0)==VK_SUCCESS);
                CHECK(vkMapMemory(device,staging_memory,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS);
            }
            struct Image { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; };
            const auto make_image=[&](VkFormat format) {   // sampled, in VK_IMAGE_LAYOUT_GENERAL, as the renderer's scene targets are
                Image result;
                VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=format; info.extent={width,height,1};
                info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL;
                info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT; info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
                CHECK(vkCreateImage(device,&info,nullptr,&result.image)==VK_SUCCESS);
                VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device,result.image,&requirements);
                VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size; allocate.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                CHECK(vkAllocateMemory(device,&allocate,nullptr,&result.memory)==VK_SUCCESS);
                CHECK(vkBindImageMemory(device,result.image,result.memory,0)==VK_SUCCESS);
                VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=result.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=format; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                CHECK(vkCreateImageView(device,&view,nullptr,&result.view)==VK_SUCCESS);
                renderer.run_commands([&](VkCommandBuffer command) {
                    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                    barrier.srcStageMask=barrier.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    barrier.srcAccessMask=barrier.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
                    barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
                    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; barrier.image=result.image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount=1; dependency.pImageMemoryBarriers=&barrier;
                    vkCmdPipelineBarrier2(command,&dependency);
                });
                return result;
            };
            const auto upload=[&](const Image& image,const void* data,size_t bytes) {
                std::memcpy(mapped,data,bytes);
                renderer.run_commands([&](VkCommandBuffer command) {
                    full_barrier(command);
                    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={width,height,1};
                    vkCmdCopyBufferToImage(command,staging,image.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
                    full_barrier(command);
                });
            };
            // Projection near 1, far 100 (the other sections' fixture): stored depth for a view distance d is depth_b/d - depth_a.
            const uc::ViewParams view{1,1,0,0,-100.f/99,-100.f/99};
            const auto distance_at=[&](uint32_t x) { return x<edge ? 2.f : 10.f; };
            Image noisy=make_image(VK_FORMAT_R32G32B32A32_SFLOAT), depth=make_image(VK_FORMAT_R32_SFLOAT), motion=make_image(VK_FORMAT_R32G32_SFLOAT), gbuffer=make_image(VK_FORMAT_R16G16B16A16_UNORM);
            {
                std::vector<float> depths(size_t(width)*height), motions(size_t(width)*height*2,0.f);
                std::vector<uint16_t> texels; texels.reserve(size_t(width)*height*4);
                for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
                    depths[size_t(y)*width+x]=view.depth_b/distance_at(x)-view.depth_a;
                    for(auto value:uc::encode_gbuffer({0,0,1},0,uc::kGBufferHasNormal)) texels.push_back(value);
                }
                upload(depth,depths.data(),depths.size()*4); upload(motion,motions.data(),motions.size()*4); upload(gbuffer,texels.data(),texels.size()*2);
            }
            uc::DenoiseInputs inputs; inputs.depth=depth.view; inputs.motion=motion.view; inputs.gbuffer=gbuffer.view; inputs.region={width,height}; inputs.view=view;
            constexpr uint32_t iterations=4;
            const auto half=[](uint16_t h) {   // IEEE half to float (the denoiser's images are RGBA16F)
                const uint32_t sign=uint32_t(h>>15)<<31, exponent=(h>>10)&31, mantissa=h&1023;
                if (!exponent) return (sign ? -1.f : 1.f)*std::ldexp(float(mantissa),-24);
                if (exponent==31) return std::bit_cast<float>(sign|0x7F800000u|(mantissa<<13));
                return std::bit_cast<float>(sign|((exponent+112)<<23)|(mantissa<<13));
            };
            // One frame: the signal (rgb = value) through the denoiser; returns the red channel of its output.
            const auto run=[&](uc::RtDenoiser& denoiser,uc::RtSignal signal,const std::vector<float>& values) {
                std::vector<float> rgba; rgba.reserve(values.size()*4);
                for(float v:values) rgba.insert(rgba.end(),{v,v,v,1.f});
                upload(noisy,rgba.data(),rgba.size()*4);
                renderer.run_commands([&](VkCommandBuffer command) { denoiser.denoise(command,signal,noisy.view,inputs,iterations); });
                renderer.run_commands([&](VkCommandBuffer command) {
                    full_barrier(command);
                    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={width,height,1};
                    vkCmdCopyImageToBuffer(command,denoiser.output_image(),VK_IMAGE_LAYOUT_GENERAL,staging,1,&copy);
                    full_barrier(command);
                });
                std::vector<float> red(values.size());
                const auto* halves=static_cast<const uint16_t*>(mapped);
                for(size_t i=0;i<red.size();++i) red[i]=half(halves[i*4]);
                return red;
            };
            const auto statistics=[](const std::vector<float>& values) {
                double sum=0,sum2=0; for(float v:values) { sum+=v; sum2+=double(v)*v; }
                const double mean=sum/values.size(); return std::array<double,2>{mean,std::sqrt(std::max(0.0,sum2/values.size()-mean*mean))};
            };
            std::mt19937 random(1234);
            std::uniform_real_distribution<float> uniform(-1.f,1.f);
            const auto noise_frame=[&]() { std::vector<float> values(size_t(width)*height); for(auto& v:values) v=0.5f+0.5f*uniform(random); return values; };
            const auto shaders=executable_dir()/"data"/"shaders";
            uc::RtDenoiser denoiser;
            denoiser.initialize(device,memory,shaders);
            denoiser.resize({width,height});
            const auto first_input=noise_frame();
            const auto first=run(denoiser,uc::RtSignal::visibility,first_input);
            std::vector<float> latest;
            for(int frame=1;frame<16;++frame) latest=run(denoiser,uc::RtSignal::visibility,noise_frame());
            const auto input_stats=statistics(first_input), first_stats=statistics(first), stats=statistics(latest);
            std::fprintf(stderr,"rt-denoise: input mean %.4f sd %.4f; first frame mean %.4f sd %.4f; after 16 frames mean %.4f sd %.4f\n",input_stats[0],input_stats[1],first_stats[0],first_stats[1],stats[0],stats[1]);
            CHECK(stats[1]<0.05 && std::abs(stats[0]-0.5)<0.02);
            // Camera cut: the history goes. A constant 0.9 right after the reset comes out as 0.9 (the old history would pull it toward 0.5), and the
            // first noisy frame again gives exactly its single-frame result.
            denoiser.reset();
            const auto cut=run(denoiser,uc::RtSignal::visibility,std::vector<float>(size_t(width)*height,0.9f));
            float cut_error=0; for(float v:cut) cut_error=std::max(cut_error,std::abs(v-0.9f));
            std::fprintf(stderr,"rt-denoise: after the cut largest difference from 0.9 %.4f\n",cut_error);
            CHECK(cut_error<0.01f);
            denoiser.reset();
            CHECK(run(denoiser,uc::RtSignal::visibility,first_input)==first);
            // A hard signal edge on the depth edge (shadowed near plane, lit far plane): no blur across it.
            denoiser.reset();
            std::vector<float> step(size_t(width)*height);
            for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) step[size_t(y)*width+x]=x<edge ? 0.f : 1.f;
            std::vector<float> stepped;
            for(int frame=0;frame<4;++frame) stepped=run(denoiser,uc::RtSignal::visibility,step);
            uint32_t widest=0;   // the farthest pixel from the edge that is neither dark nor lit
            for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
                const float v=stepped[size_t(y)*width+x];
                if(v>0.05f && v<0.95f) widest=std::max(widest,x<edge ? edge-x : x-edge+1);
                if(v<=0.05f && x>=edge+2) widest=std::max(widest,x-edge+1);   // a dark pixel more than 2 px inside the lit side
                if(v>=0.95f && x+2<edge) widest=std::max(widest,edge-x);
            }
            std::fprintf(stderr,"rt-denoise: edge spread %u px\n",widest);
            CHECK(widest<=2);
            // Radiance (variance-guided luminance weights): the same noise also averages out.
            uc::RtDenoiser radiance;
            radiance.initialize(device,memory,shaders);
            radiance.resize({width,height});
            std::vector<float> lit;
            for(int frame=0;frame<16;++frame) lit=run(radiance,uc::RtSignal::radiance,noise_frame());
            const auto radiance_stats=statistics(lit);
            std::fprintf(stderr,"rt-denoise: radiance after 16 frames mean %.4f sd %.4f\n",radiance_stats[0],radiance_stats[1]);
            CHECK(radiance_stats[1]<0.05 && std::abs(radiance_stats[0]-0.5)<0.02);
            radiance.shutdown();
            denoiser.shutdown();
            for(Image* image:{&noisy,&depth,&motion,&gbuffer}) { vkDestroyImageView(device,image->view,nullptr); vkDestroyImage(device,image->image,nullptr); vkFreeMemory(device,image->memory,nullptr); }
            vkUnmapMemory(device,staging_memory); vkDestroyBuffer(device,staging,nullptr); vkFreeMemory(device,staging_memory,nullptr);
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
