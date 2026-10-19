// GPU checks of the graphics effects (docs/superpowers/specs/2026-10-19-graphics-enhancements-design.md): one section per effect, each run by its own
// flag so a failing effect names itself. --validation turns the Vulkan validation layer on and fails on any error.
#include "check.h"
#include "gbuffer.h"
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
