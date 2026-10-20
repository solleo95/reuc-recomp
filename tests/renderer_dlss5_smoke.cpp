// GPU check of DLSS 5 Neural Rendering.
// 1. The probe says why it is unavailable (missing runtime) and, with the runtime in the directory given as the first argument, asks NGX on this GPU (feature creation;
//    the network itself runs only on a GPU the DLL is built for).
// 2. The whole pass (bridge shaders, descriptors, barriers, the three stages, passes, processing scale, failure fallback) against a stand-in for NGX that copies or fills
//    the network's output on the GPU, so the picture it produces is known. Add --validation for the Vulkan validation layer.
#include "check.h"
#include "ngx_dlss5.h"
#include "upscaler_probe.h"
#include "vulkan_renderer.h"
#include <SDL3/SDL_vulkan.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
using namespace std::chrono_literals;
namespace fs=std::filesystem;

static fs::path executable_dir() {
    std::vector<wchar_t> path(32768);
    DWORD size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!size || size >= path.size()) throw std::runtime_error("executable path unavailable");
    return fs::path(std::wstring(path.data(), size)).parent_path();
}
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

// A stand-in for NGX that does on the GPU what the test wants the "network" to do with its input.
struct Stand {
    enum Mode { copy, fill, refuse } mode=copy;
    int creates=0, evaluates=0, passes_seen=0;
    float paint[4]{.5f,.5f,.5f,1.f};   // signal-space colour the "network" outputs in fill mode
} stand;
struct Images final : uc::NgxParameters {
    std::map<std::string,uc::NgxImage> images; std::map<std::string,float> floats;
    void set_uint(const char*, uint32_t) override {}
    void set_int(const char*, int) override {}
    void set_float(const char* name, float value) override { floats[name]=value; }
    void set_image(const char* name, const uc::NgxImage& image) override { images[name]=image; }
};
struct StandBackend final : uc::NgxBackend {
    uint32_t init(const uc::NgxInit&) override { return uc::kNgxSuccess; }
    uint32_t requirements(uc::NgxRequirement&) override { return uc::kNgxSuccess; }
    std::unique_ptr<uc::NgxParameters> allocate_parameters(uint32_t& result) override { result=uc::kNgxSuccess; return std::make_unique<Images>(); }
    uint32_t create_feature(VkCommandBuffer, uc::NgxParameters&, void*& handle) override {
        ++stand.creates;
        if(stand.mode==Stand::refuse) return 0xBAD0000B;
        handle=reinterpret_cast<void*>(uintptr_t(stand.creates)); return uc::kNgxSuccess;
    }
    uint32_t evaluate(VkCommandBuffer command, void*, uc::NgxParameters& parameters) override {
        ++stand.evaluates;
        const auto& p=static_cast<Images&>(parameters);
        const auto& color=p.images.at("DLSSNR.Color"); const auto& output=p.images.at("DLSSNR.Output");
        CHECK(p.images.count("DLSSNR.Depth") && p.images.count("DLSSNR.MVec") && output.read_write && !color.read_write);
        if(stand.mode==Stand::copy) {
            VkImageCopy region{}; region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.extent={color.extent.width,color.extent.height,1};
            vkCmdCopyImage(command,color.image,VK_IMAGE_LAYOUT_GENERAL,output.image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
        } else {
            const VkClearColorValue value{{stand.paint[0],stand.paint[1],stand.paint[2],stand.paint[3]}}; const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(command,output.image,VK_IMAGE_LAYOUT_GENERAL,&value,1,&range);
        }
        return uc::kNgxSuccess;
    }
    void release_feature(void*) override {}
    void shutdown() override {}
    std::string diagnostic() const override { return "stand-in: feature refused"; }
};

int main(int argc, char** argv) {
    bool validation=false; fs::path runtime_directory;
    for(int i=1;i<argc;++i) { const std::string arg=argv[i]; if(arg=="--validation") validation=true; else runtime_directory=arg; }
    // 1. The probe. No runtime in the folder: unavailable, and the reason names the file. Nothing of NGX is started.
    const auto empty=fs::temp_directory_path()/("uc-dlss5-smoke-"+std::to_string(GetCurrentProcessId()));
    fs::create_directories(empty);
    const auto none=uc::probe_upscalers(empty);
    CHECK(!none.dlss5 && none.dlss5_reason.find(uc::kDlss5Runtime)!=std::string::npos);
    CHECK(uc::dlss5_instance_extensions(empty).empty());
    if(!runtime_directory.empty()) {
        const auto support=uc::probe_upscalers(runtime_directory);
        std::printf("DLSS 5 on this GPU: %s%s%s\n",support.dlss5 ? "available" : "unavailable",support.dlss5 ? "" : " - ",support.dlss5_reason.c_str());
        CHECK(support.dlss5 || !support.dlss5_reason.empty());   // either it works or the player is told why
        CHECK(!support.dlss5 || support.dlss5_reason.empty());
        const auto instance_extensions=uc::dlss5_instance_extensions(runtime_directory);
        std::printf("NGX instance extensions:"); for(const auto& name:instance_extensions) std::printf(" %s",name.c_str()); std::printf("\n");
        CHECK(!instance_extensions.empty());
    }
    // 2. The pass on the GPU.
    { std::ofstream(empty/uc::kDlss5Runtime,std::ios::binary) << "x"; }   // the placeholder the runtime check looks for; the stand-in replaces NGX
    SDL_Window* window=nullptr;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    window=SDL_CreateWindow("Umbrella Chronicles - DLSS 5 smoke",1280,720,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN); CHECK(window);
    ShowWindow(static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr)),SW_SHOWNOACTIVATE);
    CHECK(SDL_SyncWindow(window));
    {
        uc::VulkanRenderer renderer;
        renderer.configure_upscaling(uc::UpscalerKind::off,uc::UpscaleMode::performance,false,0.5,false,2);
        renderer.initialize(window,validation,executable_dir()/"data"/"shaders",1.0);
        CHECK(renderer.set_upscaling(uc::UpscalerKind::blit,uc::UpscaleMode::performance,false,0.5,false,2));
        std::printf("renderer DLSS 5 device: %s%s\n",renderer.dlss5_device() ? "yes" : "no - ",renderer.dlss5_device() ? "" : renderer.dlss5_status().c_str());
        uint64_t serial=0;
        const auto submit=[&](gx::FramePacket packet) {
            packet.serial=++serial;
            auto end=std::chrono::steady_clock::now()+5s;
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
        const auto frame=[&] {   // scene packet, the game's scene copy (the boundary), HUD + present: a red quad up to x=704, a green HUD quad at 832..1024
            gx::FramePacket scene; scene.present=false;
            scene.draws.push_back(quad(0x1234,-1,-1,0.1f,1,{255,0,0,255},true));
            submit(std::move(scene));
            gx::FramePacket boundary; boundary.present=false; boundary.clear_after_copy=false;
            auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->levels={{640,360,{}}};
            boundary.texture_copy=gx::TextureCopy{{0,0,640,360},target};
            submit(std::move(boundary));
            gx::FramePacket empty_packet; empty_packet.present=false; empty_packet.clear_after_copy=false;
            submit(std::move(empty_packet));
            gx::FramePacket hud; hud.present=true; hud.display_source={0,0,640,360};
            hud.draws.push_back(quad(0x5678,0.3f,-0.5f,0.6f,0.5f,{0,255,0,255},false));
            return submit(std::move(hud));
        };
        const auto picture=[&] { frame(); return renderer.capture(frame()); };
        const auto at=[](const uc::RenderPixels& pixels, uint32_t x, uint32_t y) { return &pixels.rgba[(size_t(y)*pixels.width+x)*4]; };
        // Every pixel away from an edge of `b` within the tolerance. The scene is drawn with the upscaler's jitter, which moves edges by a pixel on some frames (also with
        // DLSS 5 off), so the 6 pixels around an edge of the reference and of the picture border are not compared.
        std::vector<uint8_t> near_edge;
        const auto same=[&](const uc::RenderPixels& a, const uc::RenderPixels& b, int tolerance) {
            if(a.width!=b.width || a.height!=b.height) return false;
            const size_t w=b.width, h=b.height;
            if(near_edge.size()!=w*h) {
                std::vector<uint8_t> edge(w*h,0), spread(w*h,0);
                for(size_t y=0;y<h;++y) for(size_t x=0;x<w;++x) {
                    bool e=x==0 || y==0 || x+1==w || y+1==h;
                    for(int dy=-1;dy<=1 && !e;++dy) for(int dx=-1;dx<=1 && !e;++dx) for(int c=0;c<3 && !e;++c)
                        e=std::abs(int(b.rgba[((y+dy)*w+x+dx)*4+c])-int(b.rgba[(y*w+x)*4+c]))>8;
                    edge[y*w+x]=e;
                }
                constexpr int radius=6;
                for(size_t y=0;y<h;++y) for(size_t x=0;x<w;++x) if(edge[y*w+x]) for(int dy=-radius;dy<=radius;++dy) for(int dx=-radius;dx<=radius;++dx) {
                    const long nx=long(x)+dx, ny=long(y)+dy;
                    if(nx>=0 && ny>=0 && nx<long(w) && ny<long(h)) spread[size_t(ny)*w+size_t(nx)]=1;
                }
                near_edge=std::move(spread);
            }
            for(size_t i=0;i<a.rgba.size();++i) if(i%4!=3 && !near_edge[i/4] && std::abs(int(a.rgba[i])-int(b.rgba[i]))>tolerance) {
                std::fprintf(stderr,"pictures differ at (%zu,%zu) channel %zu: %d vs %d\n",i/4%a.width,i/4/a.width,i%4,int(a.rgba[i]),int(b.rgba[i]));
                return false;
            }
            return true;
        };
        const auto baseline=picture();
        CHECK(baseline.width==1280 && baseline.height==720 && at(baseline,400,360)[0]==255 && at(baseline,900,360)[1]==255 && at(baseline,900,360)[0]==0);
        // Without the NGX runtime next to the exe the device has no DLSS 5; the stand-in is then installed by the test
        if(!renderer.dlss5_device()) std::printf("(no NGX extensions on this device: the stand-in drives the pass)\n");
        renderer.use_dlss5_backend([]{ return std::unique_ptr<uc::NgxBackend>(new StandBackend); },empty);
        CHECK(renderer.dlss5_device());
        uc::Dlss5Settings settings;   // the defaults of the tab, with a hook chosen below
        // Hook Method Off: nothing changes and the pass never runs
        { uc::Dlss5Settings off; renderer.set_dlss5(off); const auto before=renderer.pass_runs(uc::GpuPass::dlss5); CHECK(same(picture(),baseline,0) && renderer.pass_runs(uc::GpuPass::dlss5)==before && !renderer.dlss5_active()); }
        for(const auto stage:{uc::Dlss5Hook::render,uc::Dlss5Hook::upscaled,uc::Dlss5Hook::present}) {
            std::printf("hook %u\n",unsigned(stage));
            settings.hook=stage;
            // The "network" returns its input: the picture survives the bridge (decode, gain, filters, encode) within the 8-bit rounding
            stand=Stand{}; stand.mode=Stand::copy;
            renderer.set_dlss5(settings);
            const auto before=renderer.pass_runs(uc::GpuPass::dlss5);
            const auto copy=picture();
            CHECK(renderer.dlss5_active() && renderer.dlss5_status().empty() && renderer.pass_runs(uc::GpuPass::dlss5)>before);
            CHECK(same(copy,baseline,2));
            // Half the resolution, two passes, RGB residual (the default reconstruction): the network's input comes back exactly, edges included, because the residual is zero
            uc::Dlss5Settings scaled=settings; scaled.resolution_scale=.5; scaled.passes=2; scaled.upsample_filter=uc::Dlss5UpFilter::bicubic;
            renderer.set_dlss5(scaled);
            const auto half=picture();
            CHECK(renderer.dlss5_active());
            CHECK(same(half,baseline,3));   // ping-pong parity too: an unread bridge image would be black
            // The processed image instead: only the flat areas match (the edges went through the filters)
            scaled.scale_reconstruction=uc::Dlss5Reconstruction::processed; scaled.downsample_filter=uc::Dlss5DownFilter::bilinear; scaled.scaling_domain=uc::Dlss5Domain::linear;
            renderer.set_dlss5(scaled);
            const auto processed=picture();
            for(const auto& [x,y]:{std::pair{200u,100u},std::pair{400u,360u},std::pair{600u,600u}}) for(int c=0;c<3;++c) CHECK(std::abs(int(at(processed,x,y)[c])-int(at(baseline,x,y)[c]))<=3);
            // Three passes, another size, filters and domain, the scRGB / PQ SDR encodings and an exposure value: all of it is undone on the way back
            scaled.passes=3; scaled.resolution_scale=.75; scaled.encoding=uc::Dlss5Encoding::scrgb; scaled.nr_encoding=uc::Dlss5NrEncoding::pq_sdr; scaled.scaling_domain=uc::Dlss5Domain::signed_pq;
            scaled.downsample_filter=uc::Dlss5DownFilter::nearest; scaled.upsample_filter=uc::Dlss5UpFilter::nearest; scaled.exposure_value=uc::Dlss5ExposureValue::on; scaled.exposure=2; scaled.diffuse_white_nits=500;
            renderer.set_dlss5(scaled);
            const auto three=picture();
            for(const auto& [x,y]:{std::pair{200u,100u},std::pair{400u,360u}}) for(int c=0;c<3;++c) CHECK(std::abs(int(at(three,x,y)[c])-int(at(baseline,x,y)[c]))<=3);
            // The "network" paints 0.5 in the sRGB signal encoding: stored 128 (sRGB) where it ran. The HUD is inside its picture only at the present hook.
            stand.mode=Stand::fill;
            renderer.set_dlss5(settings);
            const auto fill=picture();
            for(const auto& [x,y]:{std::pair{100u,100u},std::pair{400u,360u},std::pair{600u,650u}}) for(int c=0;c<3;++c) CHECK(std::abs(int(at(fill,x,y)[c])-128)<=2);
            if(stage==uc::Dlss5Hook::present) { for(int c=0;c<3;++c) CHECK(std::abs(int(at(fill,900,360)[c])-128)<=2); }
            else { CHECK(at(fill,900,360)[1]==255 && at(fill,900,360)[0]==0); }   // the HUD is drawn after the network
            // Switched off again: the original picture
            renderer.set_dlss5(uc::Dlss5Settings{});
            CHECK(same(picture(),baseline,0));
        }
        // The feature is refused (a GPU older than the DLL): the frame is untouched, the reason is kept, nothing is retried every frame
        stand=Stand{}; stand.mode=Stand::refuse;
        renderer.use_dlss5_backend([]{ return std::unique_ptr<uc::NgxBackend>(new StandBackend); },empty);
        settings.hook=uc::Dlss5Hook::upscaled;
        renderer.set_dlss5(settings);
        const auto refused=picture();
        CHECK(same(refused,baseline,0) && !renderer.dlss5_active());
        CHECK(renderer.dlss5_status().find("UnableToInitializeFeature")!=std::string::npos && renderer.dlss5_status().find("stand-in")!=std::string::npos);
        const int creates=stand.creates; picture(); picture();
        CHECK(stand.creates==creates && stand.evaluates==0);   // not tried again for the same settings
        CHECK(renderer.validation_errors()==0);
        renderer.shutdown();
    }
    SDL_DestroyWindow(window); SDL_Quit();
    fs::remove_all(empty);
    std::printf("renderer_dlss5_smoke passed\n");
    return 0;
}
