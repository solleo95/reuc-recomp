#include "check.h"
#include "vulkan_renderer.h"
#include "rebuild_gate.h"
#include "imgui.h"
#include <SDL3/SDL_vulkan.h>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
using namespace std::chrono_literals;

static std::filesystem::path executable_dir() {
    std::vector<wchar_t> path(32768);
    DWORD size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!size || size >= path.size()) throw std::runtime_error("executable path unavailable");
    return std::filesystem::path(std::wstring(path.data(), size)).parent_path();
}
int main(int argc, char** argv) {
    bool validation = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--validation") validation = true;
        else { std::fprintf(stderr, "usage: renderer_smoke [--validation]\n"); return 2; }
    }
    SDL_Window* window = nullptr;
    try {
        std::fprintf(stderr,"GPU smoke: initialize SDL video\n");
        bool invalid_pixels_rejected=false;
        try { uc::save_png({},{}); } catch (const std::invalid_argument&) { invalid_pixels_rejected=true; }
        CHECK(invalid_pixels_rejected);
        const auto png_path=executable_dir()/"opaque-screenshot.png";
        uc::RenderPixels png_pixels{1,2,1,{17,33,65,0,71,91,121,64}};
        uc::save_png(png_pixels,png_path);
        CHECK(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)));
        {
            Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
            CHECK(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf()))));
            Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
            CHECK(SUCCEEDED(factory->CreateDecoderFromFilename(png_path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)));
            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            CHECK(SUCCEEDED(decoder->GetFrame(0,&frame)));
            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            CHECK(SUCCEEDED(factory->CreateFormatConverter(&converter)));
            CHECK(SUCCEEDED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)));
            std::array<uint8_t,8> decoded{};
            CHECK(SUCCEEDED(converter->CopyPixels(nullptr,8,8,decoded.data())));
            CHECK((decoded==std::array<uint8_t,8>{17,33,65,255,71,91,121,255}));
        }
        CoUninitialize();
        CHECK(png_pixels.rgba[3]==0 && png_pixels.rgba[7]==64);
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
        auto invalid_window=SDL_CreateProperties(); CHECK(invalid_window);
        CHECK(SDL_SetPointerProperty(invalid_window,SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER,reinterpret_cast<void*>(uintptr_t(1))));
        CHECK(SDL_CreateWindowWithProperties(invalid_window)==nullptr);
        SDL_DestroyProperties(invalid_window);
        std::fprintf(stderr,"GPU smoke: create Vulkan window\n");
        window = SDL_CreateWindow("Umbrella Chronicles — Vulkan smoke", 320, 240, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
        if (!window) throw std::runtime_error(SDL_GetError());
        const auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
        if(!hwnd)throw std::runtime_error("smoke window has no native handle");
        ShowWindow(hwnd,SW_SHOWNOACTIVATE);
        auto resize_window=[&](int width,int height) {
            CHECK(SDL_SetWindowSize(window,width,height));
            CHECK(SDL_SyncWindow(window));
            // The renderer keeps presenting the old extent during its resize debounce.
            std::this_thread::sleep_for(uc::kRebuildInterval);
        };
        std::fprintf(stderr,"GPU smoke: initialize renderer\n");
        uc::VulkanRenderer renderer;
        gx::FramePacket packet;
        packet.draws.emplace_back();
        auto& draw = packet.draws.back(); draw.primitive = 0x90;
        draw.vertices.resize(3); draw.indices = {0,1,2};
        draw.vertices[0].position = {-8,-8,5}; draw.vertices[0].color[0] = {255,0,0,255};
        draw.vertices[1].position = {12,-8,5}; draw.vertices[1].color[0] = {0,255,0,255};
        draw.vertices[2].position = {0,8,5}; draw.vertices[2].color[0] = {0,0,255,255};
        draw.state.edit().descriptors[0]=gx::Mode::direct;
        draw.vertices[1].position_matrix=3;
        gx::set_position_matrix(draw.transforms.edit(),0,{{{0.2f,0,0,0},{0,0.2f,0,0},{0,0,-0.4f,0}}});
        gx::set_position_matrix(draw.transforms.edit(),3,{{{0.2f,0,0,-0.8f},{0,0.2f,0,0},{0,0,-0.4f,0}}});
        draw.transforms.edit().current=9; // per-vertex indices override the unloaded current matrix
        draw.transforms.edit().projection=gx::make_projection({1,0,-1,0,-1.f/9,-10.f/9},0);
        {
            uc::VulkanRenderer mesh_renderer;
            mesh_renderer.initialize(window, validation, executable_dir() / "data" / "shaders");
            auto mesh_packet=packet; mesh_packet.serial=1; mesh_packet.present=false;
            auto& mesh_draw=mesh_packet.draws[0];
            mesh_draw.mesh=std::make_shared<const gx::MeshBuffers>(gx::MeshBuffers{mesh_draw.vertices,mesh_draw.indices});
            mesh_draw.vertices.clear(); mesh_draw.indices.clear();
            CHECK(mesh_renderer.render(mesh_packet));
            // Shutdown must retire in-flight mesh storage even while its CPU owner is alive.
            mesh_renderer.shutdown();
            CHECK(mesh_renderer.validation_errors()==0);
        }
        renderer.initialize(window, validation, executable_dir() / "data" / "shaders");
        auto render = [&](uint64_t serial) {
            packet.serial = serial;
            auto end = std::chrono::steady_clock::now() + 3s;
            bool submitted = false, completed = false;
            while (!completed && std::chrono::steady_clock::now() < end) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_EVENT_QUIT) throw std::runtime_error("smoke window closed before checks finished");
                    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) renderer.resize(uint32_t(std::max(0, event.window.data1)), uint32_t(std::max(0, event.window.data2)));
                }
                if (!submitted) submitted = renderer.render(packet);
                if(submitted && !completed) CHECK(renderer.wait_for_pending(1000000));
                for (auto value : renderer.poll_completions()) { CHECK(value == serial); CHECK(!completed); completed = true; }
                std::this_thread::sleep_for(1ms);
            }
            CHECK(submitted && completed && renderer.poll_completions().empty());
            CHECK(!renderer.wait_for_pending(1000000));
            auto pixels = renderer.capture(serial);
            CHECK(pixels.width > 0 && pixels.height > 0 && pixels.rgba.size() == size_t(pixels.width) * pixels.height * 4);
            CHECK(pixels.rgba[0] == 16 && pixels.rgba[1] == 32 && pixels.rgba[2] == 64);
            size_t center = (size_t(pixels.height / 2) * pixels.width + pixels.width / 2) * 4;
            CHECK(pixels.rgba[center] >= 55 && pixels.rgba[center] <= 75);
            CHECK(pixels.rgba[center + 1] >= 55 && pixels.rgba[center + 1] <= 75);
            CHECK(pixels.rgba[center + 2] >= 115 && pixels.rgba[center + 2] <= 140 && pixels.rgba[center + 3] == 255);
            return pixels;
        };
        render(1);
        resize_window(640,360);
        renderer.resize(640, 360);
        auto pixels = render(2);
        CHECK(pixels.width == 640 && pixels.height == 360);
        renderer.resize(0, 0);
        packet.serial = 3;
        CHECK(!renderer.render(packet));
        renderer.resize(640, 360);
        pixels = render(3);
        // Readback is a real attachment capture, not a screenshot of a reference image.
        std::ofstream capture(executable_dir() / "renderer-smoke.ppm", std::ios::binary);
        capture << "P6\n" << pixels.width << ' ' << pixels.height << "\n255\n";
        for (size_t i = 0; i < pixels.rgba.size(); i += 4) capture.write(reinterpret_cast<const char*>(pixels.rgba.data() + i), 3);
        capture.close(); CHECK(capture.good());
        uc::save_png(pixels, executable_dir() / "renderer-smoke.png");
        draw.transforms.edit().position_rows[2]=draw.transforms.edit().position_rows[5]={0,0,0,-0.5f};
        draw.transforms.edit().projection=gx::make_projection({0.5f,0,-0.5f,0,1,0},1);
        // GPU slots stay active; consumer completion order follows native submission serials.
        uint64_t expected = 4;
        const auto descriptor_creations=renderer.descriptor_pool_creations();
        for (uint64_t serial = 4; serial < 20; serial += 2) {
            for (uint64_t next : {serial, serial + 1}) {
                packet.serial = next;
                auto end = std::chrono::steady_clock::now() + 3s;
                while (!renderer.render(packet)) { CHECK(std::chrono::steady_clock::now() < end); SDL_PumpEvents(); std::this_thread::sleep_for(1ms); }
            }
            auto end = std::chrono::steady_clock::now() + 3s;
            while (expected <= serial + 1) {
                CHECK(std::chrono::steady_clock::now() < end);
                for (auto completed : renderer.poll_completions()) { CHECK(completed == expected); ++expected; }
                std::this_thread::sleep_for(1ms);
            }
        }
        CHECK(renderer.presentation_count()==19);
        CHECK(renderer.descriptor_pool_creations()<=descriptor_creations+6); // one pool per frame slot
        auto submit=[&](gx::FramePacket& work,uc::VulkanRenderer* selected=nullptr) {
            auto& target=selected ? *selected : renderer;
            auto until=std::chrono::steady_clock::now()+3s;
            bool sent=false,done=false;
            while (!done) {
                CHECK(std::chrono::steady_clock::now()<until);
                if (!sent) sent=target.render(work);
                for (auto serial : target.poll_completions()) { CHECK(serial==work.serial && !done); done=true; }
                SDL_PumpEvents(); std::this_thread::sleep_for(1ms);
            }
        };
        gx::FramePacket work=packet; work.serial=20; work.present=false; work.clear_after_copy=false;
        submit(work); CHECK(renderer.presentation_count()==19);
        bool unavailable=false;
        try { renderer.capture(20); } catch (const std::runtime_error&) { unavailable=true; }
        CHECK(unavailable);
        work.serial=21; work.draws.clear(); submit(work);
        CHECK(renderer.presentation_count()==19);
        resize_window(960,540);
        renderer.resize(960,540);
        work.serial=22; work.present=true; submit(work);
        auto retained=renderer.capture(22);
        CHECK(retained.width==960 && retained.height==540);
        size_t center=(size_t(retained.height/2)*retained.width+retained.width/2)*4;
        CHECK(retained.rgba[center]>=55 && retained.rgba[center]<=75 && retained.rgba[center+2]>=115);
        CHECK(renderer.presentation_count()==20); // two finishes produced one displayed copy
        work.serial=23; work.clear_after_copy=true; work.clear_color={32,64,96,255}; submit(work);
        retained=renderer.capture(23);
        CHECK(retained.rgba[center+2]>=115); // copy precedes the EFB clear
        work.serial=24; work.present=false; work.clear_after_copy=false; submit(work);
        work.serial=25; work.present=true; submit(work);
        auto cleared=renderer.capture(25);
        CHECK(cleared.rgba[center]==32 && cleared.rgba[center+1]==64 && cleared.rgba[center+2]==96);
        CHECK(renderer.presentation_count()==22);
        auto texture=std::make_shared<gx::TexturePixels>();
        texture->levels.push_back({2,2,{255,0,0,255, 0,255,0,255, 0,0,255,255, 32,64,96,255}});
        work=packet; work.serial=26;
        work.draws[0].texture=texture;
        for (auto& vertex : work.draws[0].vertices) { vertex.color[0]={255,255,255,255}; vertex.texcoord[0]={0.75f,0.75f}; }
        submit(work);
        auto sampled=renderer.capture(26);
        CHECK(sampled.rgba[center]==32 && sampled.rgba[center+1]==64 && sampled.rgba[center+2]==96);
        for(uint64_t serial=27;serial<=31;++serial) { work.serial=serial; submit(work); }
        // Prime all six slots, then restore an old version after a full slot cycle.
        const auto texture_creations=renderer.texture_creations();
        auto changed=std::make_shared<gx::TexturePixels>(*texture);
        changed->levels[0].rgba[12]=224;
        work.draws[0].texture=changed; work.serial=32; submit(work);
        sampled=renderer.capture(32); CHECK(sampled.rgba[center]==224);
        const auto after_changed=renderer.texture_creations(); CHECK(after_changed>=texture_creations+1);
        work.draws[0].texture=texture;
        for(uint64_t serial=33;serial<=37;++serial) { work.serial=serial; submit(work); }
        work.serial=38; submit(work);
        sampled=renderer.capture(38); CHECK(sampled.rgba[center]==32);
        CHECK(renderer.texture_creations()==after_changed); // the original version survives replacement in every slot
        for (auto& vertex : work.draws[0].vertices) vertex.texcoord[0]={0.25f,0.25f};
        work.serial=39; submit(work);
        sampled=renderer.capture(39); CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0 && sampled.rgba[center+2]==0);
        auto invalid=std::make_shared<gx::TexturePixels>(*texture); invalid->levels[0].rgba.pop_back();
        work.draws[0].texture=invalid; work.serial=40;
        bool invalid_texture=false;
        try { renderer.render(work); } catch (const std::invalid_argument&) { invalid_texture=true; }
        CHECK(invalid_texture);
        invalid=std::make_shared<gx::TexturePixels>(*texture); invalid->levels.push_back({2,1,{255,255,255,255,255,255,255,255}});
        work.draws[0].texture=invalid; invalid_texture=false;
        try { renderer.render(work); } catch (const std::invalid_argument&) { invalid_texture=true; }
        CHECK(invalid_texture);
        work=packet; work.serial=40; work.draws[0].texture.reset();
        work.draws[0].blend={1,4,5,3,true,true};
        for (auto& vertex:work.draws[0].vertices) vertex.color[0]={255,0,0,0};
        submit(work); sampled=renderer.capture(40);
        CHECK(sampled.rgba[center]==16 && sampled.rgba[center+1]==32 && sampled.rgba[center+2]==64);
        work.serial=41;
        for (auto& vertex:work.draws[0].vertices) vertex.color[0]={255,0,0,128};
        submit(work); sampled=renderer.capture(41);
        CHECK(sampled.rgba[center]>=134 && sampled.rgba[center]<=137 && sampled.rgba[center+1]>=15 && sampled.rgba[center+1]<=17);
        work=packet; work.serial=42;
        auto red=std::make_shared<gx::TexturePixels>(); red->levels.push_back({1,1,{255,0,0,255}});
        auto mask=std::make_shared<gx::TexturePixels>(); mask->levels.push_back({1,1,{0,255,0,64}});
        work.draws[0].texture=red; work.draws[0].texture_maps[0]=mask;
        auto& material=work.draws[0].material; material.stages=2; material.swaps={0xE4,0xE4,0xE4,0xE4};
        material.stage[0]={0x8FFF8,0x8FFC0,0x3C0,0,0};
        material.stage[1]={0x8FFF0,0x8FFC0,0x3C1,0,0};
        submit(work); sampled=renderer.capture(42);
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0 && sampled.rgba[center+2]==0 && sampled.rgba[center+3]==64);
        for(uint64_t serial=43;serial<=47;++serial) { work.serial=serial; submit(work); } // populate all six slots
        const auto before_copy_only=renderer.texture_creations();
        auto copy_only=work;
        // A nonempty postprocessing/UI batch must also retain the scene's immutable textures.
        copy_only.draws[0].texture.reset(); copy_only.draws[0].texture_maps={}; copy_only.draws[0].material.stages=0;
        copy_only.serial=48; submit(copy_only);
        copy_only.serial=49; submit(copy_only);
        work.serial=50; submit(work);
        CHECK(renderer.texture_creations()==before_copy_only);
        sampled=renderer.capture(50);
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+3]==64);
        work.serial=51; work.draws[0].uniforms.edit().pixel[0]=0; submit(work);
        sampled=renderer.capture(51); CHECK(sampled.rgba[center]==16 && sampled.rgba[center+1]==32);
        work.serial=52; work.draws[0].uniforms.edit().pixel={0x3F0000,0,256|123,2}; submit(work);
        sampled=renderer.capture(52); CHECK(sampled.rgba[center]==255 && sampled.rgba[center+3]==123);
        work.draws[0].uniforms.edit().pixel={0x3F0000,31,0,2};
        auto hidden=work.draws[0]; hidden.material.stages=0; hidden.texture.reset(); hidden.uniforms.edit().pixel[1]=17;
        for(auto& vertex:hidden.vertices) vertex.color[0]={0,255,0,255};
        work.draws.push_back(hidden); work.serial=53; submit(work);
        sampled=renderer.capture(53); CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0 && sampled.rgba[center+3]==64);
        work.draws.resize(1); work.serial=54;
        work.draws[0].blend={1,4,5,3,true,true}; work.draws[0].uniforms.edit().pixel={0x3F0000,0,256|123,2};
        submit(work); sampled=renderer.capture(54);
        CHECK(sampled.rgba[center]>=75 && sampled.rgba[center]<=77 && sampled.rgba[center+1]>=23 && sampled.rgba[center+1]<=25 && sampled.rgba[center+3]==123);
        work=packet; work.serial=55;
        auto mips=std::make_shared<gx::TexturePixels>();
        mips->levels.push_back({2,2,{255,0,0,255,255,0,0,255,255,0,0,255,255,0,0,255}});
        mips->levels.push_back({1,1,{17,33,65,255}}); work.draws[0].texture=mips;
        for(auto& vertex:work.draws[0].vertices) vertex.color[0]={255,255,255,255};
        work.draws[0].vertices[0].texcoord[0]={0,0}; work.draws[0].vertices[1].texcoord[0]={512,0}; work.draws[0].vertices[2].texcoord[0]={0,512};
        work.draws[0].texture_wrap[0]=0x20; // Explicit nearest mip selection.
        submit(work); sampled=renderer.capture(55);
        CHECK(sampled.rgba[center]==17 && sampled.rgba[center+1]==33 && sampled.rgba[center+2]==65);
        auto old_copy=std::make_shared<gx::TexturePixels>(); old_copy->gpu_only=true;
        old_copy->levels={{4,4,{}},{2,2,{}},{1,1,{}}};
        auto new_copy=std::make_shared<gx::TexturePixels>(*old_copy);
        work=packet; work.serial=56; work.present=false; work.clear_color={0,255,0,255};
        auto quad=work.draws[0]; quad.material.stages=0; quad.primitive=0x90;
        quad.transforms=gx::Transforms{}; quad.state.edit().descriptors[0]=gx::Mode::none;
        quad.vertices.resize(4); quad.indices={0,1,2,0,2,3};
        for(auto& vertex:quad.vertices) vertex.color[0]={255,0,0,255};
        quad.vertices[0].position={-1,-1,0}; quad.vertices[1].position={1,-1,0};
        quad.vertices[2].position={1,1,0}; quad.vertices[3].position={-1,1,0};
        work.draws[0]=quad;
        work.texture_copy=gx::TextureCopy{{0,0,640,480},old_copy}; submit(work);
        work.serial=57; work.draws.clear(); work.clear_color={0,0,255,255};
        work.texture_copy=gx::TextureCopy{{0,0,640,480},new_copy}; submit(work);
        work=packet; work.serial=58; work.draws[0].texture=old_copy;
        for(auto& vertex:work.draws[0].vertices) { vertex.color[0]={255,255,255,255}; vertex.texcoord[0]={0.5f,0.5f}; }
        work.draws[0].vertices[0].texcoord[0]={0,0}; work.draws[0].vertices[1].texcoord[0]={512,0}; work.draws[0].vertices[2].texcoord[0]={0,512};
        submit(work); sampled=renderer.capture(58);
        std::fprintf(stderr,"EFB old-copy pixel %u %u %u %u\n",sampled.rgba[center],sampled.rgba[center+1],sampled.rgba[center+2],sampled.rgba[center+3]);
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0);
        work.serial=59; work.draws[0].texture=new_copy; submit(work); sampled=renderer.capture(59);
        CHECK(sampled.rgba[center]==0 && sampled.rgba[center+1]==255);
        resize_window(1024,576); renderer.resize(1024,576);
        work.serial=60; work.draws[0].texture=old_copy; submit(work); sampled=renderer.capture(60);
        center=(size_t(sampled.height/2)*sampled.width+sampled.width/2)*4;
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0);
        auto ephemeral=std::make_shared<gx::TexturePixels>(); ephemeral->levels={{1,1,{17,33,65,255}}};
        std::weak_ptr<const gx::TexturePixels> released=ephemeral;
        work.serial=61; work.draws[0].texture=ephemeral; submit(work);
        work.serial=62; submit(work); // Both slots own this version.
        ephemeral.reset(); work.draws[0].texture.reset();
        work.serial=63; submit(work); CHECK(!released.expired());
        for(uint64_t serial=64;serial<=68;++serial) { work.serial=serial; submit(work); }
        CHECK(released.expired());
        work=packet; work.serial=69;
        work.draws[0]=quad; // A full-screen red quad, limited by native viewport and scissor.
        work.draws[0].state.edit().viewport=std::array<float,6>{0,0,320,480,0,1};
        work.draws[0].state.edit().scissor=std::array<int32_t,4>{0,0,160,480};
        submit(work); sampled=renderer.capture(69);
        const auto inside=(size_t(sampled.height/2)*sampled.width+sampled.width/8)*4;
        const auto clipped=(size_t(sampled.height/2)*sampled.width+sampled.width*3/8)*4;
        CHECK(sampled.rgba[inside]==255 && sampled.rgba[inside+1]==0);
        CHECK(sampled.rgba[clipped]==16 && sampled.rgba[clipped+1]==32);
        bool rejected = false;
        work=packet; work.serial=70; work.draws[0]=quad;
        work.draws[0].state.edit().viewport=std::array<float,6>{0,0,640,360,0,1};
        work.display_source=std::array<uint32_t,4>{0,0,640,360};
        submit(work); sampled=renderer.capture(70);
        const auto bottom=(size_t(sampled.height*7/8)*sampled.width+sampled.width/2)*4;
        CHECK(sampled.rgba[bottom]==255 && sampled.rgba[bottom+1]==0);
        auto wrapped=std::make_shared<gx::TexturePixels>(); wrapped->levels={{2,1,{255,0,0,255,0,0,255,255}}};
        work=packet; work.serial=71; work.draws[0]=quad; work.draws[0].texture=wrapped;
        for(auto& vertex:work.draws[0].vertices) { vertex.color[0]={255,255,255,255}; vertex.texcoord[0]={1.1f,0.5f}; }
        submit(work); sampled=renderer.capture(71); CHECK(sampled.rgba[center]==0 && sampled.rgba[center+2]==255);
        work.serial=72; work.draws[0].texture_wrap[0]=1;
        submit(work); sampled=renderer.capture(72); CHECK(sampled.rgba[center]==255 && sampled.rgba[center+2]==0);
        work.serial=73; work.draws[0].texture_wrap[0]=2;
        submit(work); sampled=renderer.capture(73); CHECK(sampled.rgba[center]==0 && sampled.rgba[center+2]==255);
        work.serial=74; work.draws[0].texture_maps[0]=wrapped;
        work.draws[0].material.stages=1; work.draws[0].material.swaps={0xE4,0xE4,0xE4,0xE4};
        work.draws[0].material.stage[0]={0x8FFF8,0x8FF80,0x41,0,0};
        work.draws[0].texture_wrap[0]=0; work.draws[0].texture_wrap[1]=1;
        submit(work); sampled=renderer.capture(74); CHECK(sampled.rgba[center]==255 && sampled.rgba[center+2]==0);
        work.serial=75; work.draws[0].texture_wrap[1]=0;
        submit(work); sampled=renderer.capture(75); CHECK(sampled.rgba[center]==0 && sampled.rgba[center+2]==255);
        auto partial_copy=std::make_shared<gx::TexturePixels>(); partial_copy->gpu_only=true; partial_copy->levels={{4,4,{}}};
        work=packet; work.serial=76; work.present=false; work.draws[0]=quad; work.clear_color={0,255,0,255};
        work.texture_copy=gx::TextureCopy{{0,0,320,480},partial_copy}; submit(work);
        auto intermediate=renderer.capture(76,true);
        const auto efb_left=(size_t(intermediate.height/2)*intermediate.width+intermediate.width/4)*4;
        const auto efb_right=(size_t(intermediate.height/2)*intermediate.width+intermediate.width*3/4)*4;
        CHECK(intermediate.rgba[efb_left]==0 && intermediate.rgba[efb_left+1]==255);
        CHECK(intermediate.rgba[efb_right]==255 && intermediate.rgba[efb_right+1]==0);
        work=packet; work.serial=77; work.draws.clear(); work.clear_after_copy=false;
        submit(work); sampled=renderer.capture(77);
        bool stale_efb=false;
        try { renderer.capture(76,true); } catch(const std::invalid_argument&) { stale_efb=true; }
        CHECK(stale_efb);
        const auto left=(size_t(sampled.height/2)*sampled.width+sampled.width/4)*4;
        const auto right=(size_t(sampled.height/2)*sampled.width+sampled.width*3/4)*4;
        CHECK(sampled.rgba[left]==0 && sampled.rgba[left+1]==255);
        CHECK(sampled.rgba[right]==255 && sampled.rgba[right+1]==0);
        work=packet; work.serial=78; work.draws[0]=quad; work.draws[0].texture=wrapped;
        for(auto& vertex:work.draws[0].vertices) { vertex.color[0]={255,255,255,255}; vertex.texcoord[0]={0.1f,0.5f}; }
        work.draws[0].uniforms.edit().tex_rows[0]={1,0,0.75f,0}; // AB11 uses both homogeneous components
        submit(work); sampled=renderer.capture(78);
        CHECK(sampled.rgba[center]==0 && sampled.rgba[center+2]==255);
        work=packet; work.serial=79; work.draws[0]=quad;
        work.draws[0].uniforms.edit().channels={2,1,0,1};
        work.draws[0].uniforms.edit().materials[0]={1,1,1,1};
        work.draws[0].uniforms.edit().ambient[0]={9/255.f,9/255.f,9/255.f,1};
        submit(work); sampled=renderer.capture(79);
        std::fprintf(stderr,"native ambient pixel %u %u %u %u\n",sampled.rgba[center],sampled.rgba[center+1],sampled.rgba[center+2],sampled.rgba[center+3]);
        CHECK(sampled.rgba[center]==8 && sampled.rgba[center+1]==8 && sampled.rgba[center+2]==8); // 255*9 >> 8
        work.serial=80; work.draws[0].uniforms.edit().channels[0]=6;
        work.draws[0].uniforms.edit().lights[0].color={128,128,128,255};
        work.draws[0].uniforms.edit().lights[0].position={0,0,1,0};
        submit(work); sampled=renderer.capture(80);
        CHECK(sampled.rgba[center]==137 && sampled.rgba[center+1]==137);
        work=packet; work.serial=81; work.draws[0]=quad; work.draws[0].state.edit().cull=3;
        submit(work); sampled=renderer.capture(81);
        CHECK(sampled.rgba[center]==16 && sampled.rgba[center+1]==32 && sampled.rgba[center+2]==64);
        work=packet; work.serial=82; work.draws[0]=quad; work.draws[0].uniforms.edit().pixel[3]=0;
        submit(work); sampled=renderer.capture(82);
        CHECK(sampled.rgba[center]==0 && sampled.rgba[center+1]==0 && sampled.rgba[center+2]==0);
        work=packet; work.serial=83; work.draws[0]=quad; work.draws[0].texture=wrapped;
        for(auto& vertex:work.draws[0].vertices) { vertex.color[0]={255,255,255,255}; vertex.texcoord[0]={0.5f,0.5f}; }
        work.draws[0].texture_wrap[0]=0x10; // Linear magnification blends the two texels.
        submit(work); sampled=renderer.capture(83);
        CHECK(sampled.rgba[center]>=127 && sampled.rgba[center]<=128 && sampled.rgba[center+2]>=127 && sampled.rgba[center+2]<=128);
        work=packet; work.serial=84; work.draws[0]=quad; work.draws[0].texture=mips;
        for(auto& vertex:work.draws[0].vertices) vertex.color[0]={255,255,255,255};
        work.draws[0].vertices[0].texcoord[0]={0,0}; work.draws[0].vertices[1].texcoord[0]={512,0};
        work.draws[0].vertices[2].texcoord[0]={512,512}; work.draws[0].vertices[3].texcoord[0]={0,512};
        submit(work); sampled=renderer.capture(84);
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0 && sampled.rgba[center+2]==0); // Mips exist but this sampler disables them.
        const auto pools_before_growth=renderer.descriptor_pool_creations();
        work=packet; work.draws.assign(8,quad);
        for(uint64_t serial=85;serial<=90;++serial) { work.serial=serial; submit(work); }
        CHECK(renderer.descriptor_pool_creations()==pools_before_growth+6);
        const auto pools_after_growth=renderer.descriptor_pool_creations();
        work.draws.resize(1); work.serial=91; submit(work);
        work.draws.clear(); work.serial=92; submit(work);
        work.draws.assign(8,quad); work.serial=93; submit(work);
        CHECK(renderer.descriptor_pool_creations()==pools_after_growth);
        sampled=renderer.capture(93); CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0);
        uint64_t warmed_copy_allocations=0;
        for(uint64_t serial=94;serial<106;++serial) {
            auto version=std::make_shared<gx::TexturePixels>(); version->gpu_only=true;
            version->levels={{4,4,{}},{2,2,{}},{1,1,{}}};
            work=packet; work.serial=serial; work.present=false; work.clear_after_copy=false; work.draws[0]=quad;
            for(auto& vertex:work.draws[0].vertices) vertex.color[0]=serial&1 ? std::array<uint8_t,4>{0,255,0,255} : std::array<uint8_t,4>{255,0,0,255};
            work.texture_copy=gx::TextureCopy{{0,0,640,480},version}; submit(work);
            work.texture_copy.reset(); version.reset();
            if(serial==101) warmed_copy_allocations=renderer.texture_creations();
            if(serial>101) CHECK(renderer.texture_creations()==warmed_copy_allocations);
        }
        work=packet; work.serial=106; work.draws[0]=quad; work.draws[0].texture=old_copy;
        for(auto& vertex:work.draws[0].vertices) { vertex.color[0]={255,255,255,255}; vertex.texcoord[0]={.5f,.5f}; }
        submit(work); sampled=renderer.capture(106);
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0); // Retained version survived allocation recycling.
        auto broken_copy=std::make_shared<gx::TexturePixels>(); broken_copy->gpu_only=true; broken_copy->levels={{4,4,{}},{3,2,{}},{1,1,{}}};
        work=packet; work.serial=107; work.present=false; work.texture_copy=gx::TextureCopy{{0,0,640,480},broken_copy};
        bool malformed_copy_rejected=false;
        try { renderer.render(work); } catch(const std::invalid_argument&) { malformed_copy_rejected=true; }
        CHECK(malformed_copy_rejected);
        renderer.set_render_scale(1);
        std::this_thread::sleep_for(uc::kRebuildInterval);
        work=packet; work.serial=107; work.clear_after_copy=false; work.draws.assign(8,quad);
        for(size_t i=0;i<work.draws.size();++i) {
            work.draws[i].state.edit().scissor=std::array<int32_t,4>{int32_t(i*80),0,80,480};
            for(auto& vertex:work.draws[i].vertices) vertex.color[0]=i&1 ? std::array<uint8_t,4>{0,255,0,255} : std::array<uint8_t,4>{255,0,0,255};
        }
        submit(work);
        auto high_resolution=renderer.capture(107,true);
        CHECK(high_resolution.width==1280 && high_resolution.height==960);
        auto scaled_copy=std::make_shared<gx::TexturePixels>(); scaled_copy->gpu_only=true; scaled_copy->levels={{4,4,{}},{2,2,{}},{1,1,{}}};
        work.serial=108; work.present=false; work.texture_copy=gx::TextureCopy{{0,0,640,480},scaled_copy}; submit(work);
        renderer.set_render_scale(3);
        std::this_thread::sleep_for(uc::kRebuildInterval);
        work=packet; work.serial=109; work.clear_after_copy=false; work.draws[0]=quad; work.draws[0].texture=scaled_copy;
        for(auto& vertex:work.draws[0].vertices) { vertex.color[0]={255,255,255,255}; vertex.texcoord[0]={.5f,.5f}; }
        work.draws[0].vertices[0].texcoord[0][0]=work.draws[0].vertices[3].texcoord[0][0]=0;
        work.draws[0].vertices[1].texcoord[0][0]=work.draws[0].vertices[2].texcoord[0][0]=1;
        submit(work); high_resolution=renderer.capture(109,true);
        CHECK(high_resolution.width==3840 && high_resolution.height==2880);
        for(size_t i=0;i<8;++i) {
            const size_t texel=(size_t(high_resolution.height/2)*high_resolution.width+(2*i+1)*high_resolution.width/16)*4;
            CHECK(high_resolution.rgba[texel]==(i&1 ? 0 : 255) && high_resolution.rgba[texel+1]==(i&1 ? 255 : 0));
        } // Eight physical texels survive a logical four-texel copy and scale change.
        renderer.set_render_scale(1.5);
        std::this_thread::sleep_for(uc::kRebuildInterval);
        work=packet; work.serial=110; work.clear_after_copy=false; work.draws[0]=quad; submit(work);
        high_resolution=renderer.capture(110,true);
        CHECK(high_resolution.width==1920 && high_resolution.height==1440);
        renderer.set_render_scale(6);
        std::this_thread::sleep_for(uc::kRebuildInterval);
        work.serial=111; submit(work); high_resolution=renderer.capture(111,true);
        CHECK(high_resolution.width==7680 && high_resolution.height==5760);
        resize_window(1280,720); renderer.resize(1280,720);
        work.serial=112; submit(work); high_resolution=renderer.capture(112,true);
        CHECK(high_resolution.width==7680 && high_resolution.height==5760); // Manual internal resolution survives output resize.
        renderer.set_render_scale(1.25);
        std::this_thread::sleep_for(uc::kRebuildInterval);
        work.serial=113; work.draws[0].state.edit().viewport=std::array<float,6>{0,0,640,360,0,1}; work.display_source={0,0,640,360};
        submit(work); sampled=renderer.capture(113);
        CHECK(sampled.width==1280 && sampled.height==720);
        CHECK(sampled.rgba[(size_t(sampled.height-1)*sampled.width+sampled.width/2)*4]==255);
        high_resolution=renderer.capture(113,true);
        CHECK(high_resolution.width==1600 && high_resolution.height==1200);
        renderer.set_render_scale(.5);
        std::this_thread::sleep_for(uc::kRebuildInterval);
        auto alpha_copy=std::make_shared<gx::TexturePixels>(); alpha_copy->gpu_only=true;
        alpha_copy->channels=gx::TextureChannels::alpha_intensity; alpha_copy->levels={{4,4,{}},{2,2,{}},{1,1,{}}};
        work=packet; work.serial=114; work.present=false; work.draws[0]=quad;
        work.draws[0].uniforms.edit().pixel[2]=256|77;
        for(auto& vertex:work.draws[0].vertices)vertex.color[0]={17,33,65,255};
        work.texture_copy=gx::TextureCopy{{0,0,640,480},alpha_copy}; submit(work);
        work=packet; work.serial=115; work.draws[0]=quad; work.draws[0].material=packet.draws[0].material;
        work.draws[0].texture=alpha_copy;
        for(auto& vertex:work.draws[0].vertices) {vertex.color[0]={255,255,255,255};vertex.texcoord[0]={.5f,.5f};}
        submit(work); sampled=renderer.capture(115);
        center=(size_t(sampled.height/2)*sampled.width+sampled.width/2)*4;
        CHECK(sampled.rgba[center]==77 && sampled.rgba[center+1]==77 && sampled.rgba[center+2]==77 && sampled.rgba[center+3]==77);
        // Dropping an alpha-only version must never recycle its swizzled view as RGBA.
        alpha_copy.reset(); work.draws[0].texture.reset(); work.serial=116; submit(work);
        auto rgba_copy=std::make_shared<gx::TexturePixels>(); rgba_copy->gpu_only=true; rgba_copy->levels={{4,4,{}},{2,2,{}},{1,1,{}}};
        work=packet; work.serial=117; work.present=false; work.draws[0]=quad;
        for(auto& vertex:work.draws[0].vertices)vertex.color[0]={17,33,65,255};
        work.texture_copy=gx::TextureCopy{{0,0,640,480},rgba_copy}; submit(work);
        work=packet; work.serial=118; work.draws[0]=quad; work.draws[0].material=packet.draws[0].material; work.draws[0].texture=rgba_copy;
        for(auto& vertex:work.draws[0].vertices) {vertex.color[0]={255,255,255,255};vertex.texcoord[0]={.5f,.5f};}
        submit(work); sampled=renderer.capture(118);
        CHECK(sampled.rgba[center]==17 && sampled.rgba[center+1]==33 && sampled.rgba[center+2]==65);
        work=packet; work.serial=119; work.draws[0]=quad; work.clear_color={0,0,0,255};
        auto& line=work.draws[0]; line.primitive=0xA8;
        line.vertices={quad.vertices[0],quad.vertices[2]}; line.indices={0,1};
        for(auto& vertex:line.vertices)vertex.color[0]={255,0,0,255};
        submit(work); sampled=renderer.capture(119);
        size_t line_pixels=0;
        for(size_t i=0;i<sampled.rgba.size();i+=4)if(sampled.rgba[i]==255 && sampled.rgba[i+1]==0)++line_pixels;
        CHECK(line_pixels>100 && line_pixels<5000);
        work.serial=120; line.primitive=0xB0; submit(work); sampled=renderer.capture(120);
        size_t strip_pixels=0;
        for(size_t i=0;i<sampled.rgba.size();i+=4)if(sampled.rgba[i]==255 && sampled.rgba[i+1]==0)++strip_pixels;
        CHECK(strip_pixels==line_pixels);
        work.serial=121; line.indices.push_back(0); bool odd_line_rejected=false;
        try {renderer.render(work);}catch(const std::invalid_argument&){odd_line_rejected=true;}
        CHECK(odd_line_rejected);
        work=packet; work.serial=121; work.draws[0]=quad;
        submit(work); sampled=renderer.capture(121);
        CHECK(sampled.rgba[center]==255); // Triangle pipeline survives line topology cache entries.
        auto rgb565_copy=std::make_shared<gx::TexturePixels>(); rgb565_copy->gpu_only=true;
        rgb565_copy->channels=gx::TextureChannels::rgb565; rgb565_copy->levels={{4,4,{}},{2,2,{}},{1,1,{}}};
        work=packet; work.serial=122; work.present=false; work.draws[0]=quad;
        for(auto& vertex:work.draws[0].vertices)vertex.color[0]={17,33,65,19};
        work.texture_copy=gx::TextureCopy{{0,0,640,480},rgb565_copy}; submit(work);
        work=packet; work.serial=123; work.draws[0]=quad; work.draws[0].material=packet.draws[0].material; work.draws[0].texture=rgb565_copy;
        for(auto& vertex:work.draws[0].vertices) {vertex.color[0]={255,255,255,255};vertex.texcoord[0]={.5f,.5f};}
        submit(work); sampled=renderer.capture(123);
        CHECK(sampled.rgba[center]==16 && sampled.rgba[center+1]==32 && sampled.rgba[center+2]==66 && sampled.rgba[center+3]==255);
        work={};work.serial=124;work.clear_color={16,32,64,255};submit(work);
        const auto prompt_background=renderer.capture(124);
        const auto prompt_efb_background=renderer.capture(124,true);
        work.serial=125;
        work.prompts.push_back({uc::describe_prompt(uc::default_bindings(),uc::Action::interact),{160,120,480,120,480,360,160,360}});
        submit(work);sampled=renderer.capture(125);
        CHECK(sampled.rgba!=prompt_background.rgba);
        CHECK(std::equal(sampled.rgba.begin(),sampled.rgba.begin()+4,prompt_background.rgba.begin()));
        CHECK(renderer.capture(125,true).rgba==prompt_efb_background.rgba);
        uc::save_png(sampled,executable_dir()/"native-prompts.png");
        work.serial=126;work.prompts[0].alpha={0,0,0,0};submit(work);
        CHECK(renderer.capture(126).rgba==prompt_background.rgba);
        work.serial=127;work.prompts.clear();
        const auto assignments=uc::default_bindings();
        const uc::Action prompt_actions[]={uc::Action::grenade,uc::Action::knife,uc::Action::weapon_previous,uc::Action::weapon_next,uc::Action::confirm,uc::Action::qte_shake};
        for(size_t i=0;i<6;++i) {
            const float x=20+float(i%2)*320,y=20+float(i/2)*150;
            work.prompts.push_back({uc::describe_prompt(assignments,prompt_actions[i]),{x,y,x+280,y,x+280,y+120,x,y+120}});
        }
        submit(work);sampled=renderer.capture(127);
        CHECK(sampled.rgba!=prompt_background.rgba);
        uc::save_png(sampled,executable_dir()/"native-prompt-cases.png");
        work.serial=128;work.prompts.resize(1);
        auto cleared_assignments=assignments;cleared_assignments[size_t(uc::Action::grenade)].clear();
        work.prompts[0]={uc::describe_prompt(cleared_assignments,uc::Action::grenade),{320,40,520,180,320,320,120,180},{.5f,.5f,.5f,.5f},{100,100,540,260}};
        submit(work);sampled=renderer.capture(128);
        CHECK(sampled.rgba!=prompt_background.rgba);
        CHECK(std::equal(sampled.rgba.begin(),sampled.rgba.begin()+4,prompt_background.rgba.begin()));
        work.serial=129;work.prompts[0].alpha[0]=2;
        bool bad_prompt=false;try{renderer.render(work);}catch(const std::invalid_argument&){bad_prompt=true;}
        CHECK(bad_prompt);work.prompts[0].alpha={1,1,1,1};
        size_t previous_prompt_pixels=0;
        for(const auto output: {VkExtent2D{3840,2160},VkExtent2D{7680,4320}}) {
            resize_window(int(output.width),int(output.height));
            renderer.resize(output.width,output.height);submit(work);sampled=renderer.capture(work.serial++);
            CHECK(sampled.width==output.width && sampled.height==output.height);
            size_t changed=0;for(size_t i=0;i<sampled.rgba.size();i+=4)if(sampled.rgba[i]!=16 || sampled.rgba[i+1]!=32 || sampled.rgba[i+2]!=64)++changed;
            std::fprintf(stderr,"Native prompt output %ux%u: changed pixels %zu\n",sampled.width,sampled.height,changed);
            CHECK(changed>100 && changed<size_t(output.width)*output.height/2);
            CHECK(changed>previous_prompt_pixels*2);previous_prompt_pixels=changed;
        }
        resize_window(1280,720);renderer.resize(1280,720);
        for(uint64_t serial:{131u,132u}) {
            work.serial=serial;auto until=std::chrono::steady_clock::now()+3s;
            while(!renderer.render(work)) {CHECK(std::chrono::steady_clock::now()<until);std::this_thread::sleep_for(1ms);}
        }
        uint64_t prompt_complete=131;auto prompt_deadline=std::chrono::steady_clock::now()+3s;
        while(prompt_complete<=132) {
            CHECK(std::chrono::steady_clock::now()<prompt_deadline);
            for(auto serial:renderer.poll_completions()) {CHECK(serial==prompt_complete);++prompt_complete;}
            std::this_thread::sleep_for(1ms);
        }
        // Inline text glyph cell: primary binding only, drawn inside the 24x26 EFB cell.
        auto inline_cell=[&](uint64_t serial,const uc::PromptDescription& description,size_t minimum) {
            work.serial=serial;work.prompts={{description,{300,200,324,200,324,226,300,226}}};work.prompts[0].compact=true;
            submit(work);auto pixels=renderer.capture(serial);
            size_t inside=0,outside=0;
            for(uint32_t y=0;y<pixels.height;++y)for(uint32_t x=0;x<pixels.width;++x) {
                const size_t i=(size_t(y)*pixels.width+x)*4;
                if(pixels.rgba[i]==16 && pixels.rgba[i+1]==32 && pixels.rgba[i+2]==64)continue;
                const bool cell=x>=598 && x<=650 && y>=298 && y<=341;
                (cell ? inside : outside)++;
            }
            std::fprintf(stderr,"Native inline glyph: inside %zu outside %zu\n",inside,outside);
            CHECK(inside>minimum && outside==0);
            uc::save_png(pixels,executable_dir()/("native-inline-"+std::to_string(serial)+".png"));
            uc::RenderPixels detail{serial,480,440,std::vector<uint8_t>(480*440*4)};
            for(uint32_t y=0;y<440;++y)for(uint32_t x=0;x<480;++x) {
                const size_t source=(size_t(292+y/8)*pixels.width+595+x/8)*4,target=(size_t(y)*480+x)*4;
                std::copy_n(pixels.rgba.begin()+source,4,detail.rgba.begin()+target);
            }
            uc::save_png(detail,executable_dir()/("native-inline-detail-"+std::to_string(serial)+".png"));
            return inside;
        };
        inline_cell(133,uc::primary_prompt(assignments,{uc::Action::interact}),150);
        const auto monochrome_mouse=renderer.capture(133);
        inline_cell(134,uc::primary_prompt(assignments,{uc::Action::weapon_previous,uc::Action::weapon_next}),150);
        auto cleared_interact=assignments;cleared_interact[size_t(uc::Action::interact)].clear();
        inline_cell(135,uc::primary_prompt(cleared_interact,{uc::Action::interact}),10);
        renderer.set_prompt_style(uc::PromptStyle::sculpted);
        inline_cell(136,uc::primary_prompt(assignments,{uc::Action::interact}),150);
        const auto sculpted_left=renderer.capture(136);
        CHECK(sculpted_left.rgba!=monochrome_mouse.rgba);
        bool invalid_prompt_style=false;
        try{renderer.set_prompt_style(uc::PromptStyle(99));}catch(const std::invalid_argument&){invalid_prompt_style=true;}
        CHECK(invalid_prompt_style);
        inline_cell(137,uc::primary_prompt(assignments,{uc::Action::fire}),150);
        const auto sculpted_right=renderer.capture(137);
        auto active_pixels=[](const uc::RenderPixels& pixels) {
            // Weight antialiased cyan coverage; exact-color counts depend on subpixel placement.
            double count=0;for(size_t i=0;i<pixels.rgba.size();i+=4) {
                const int chroma=int(pixels.rgba[i+1])-pixels.rgba[i];
                if(chroma>25)count+=chroma/105.;
            }
            return count;
        };
        const auto left_fill=active_pixels(sculpted_left),right_fill=active_pixels(sculpted_right);
        std::fprintf(stderr,"Native sculpted fill: left %.2f right %.2f\n",left_fill,right_fill);
        CHECK(std::min(left_fill,right_fill)>20);
        CHECK(std::max(left_fill,right_fill)-std::min(left_fill,right_fill)<std::max(left_fill,right_fill)*.05+1);
        for(const auto* pixels:{&monochrome_mouse,&sculpted_left,&sculpted_right})
            for(auto xy:{std::pair{602u,301u},{646u,301u},{602u,337u},{646u,337u}}) {
                const size_t i=(size_t(xy.second)*pixels->width+xy.first)*4;
                CHECK(pixels->rgba[i]==16 && pixels->rgba[i+1]==32 && pixels->rgba[i+2]==64);
            }
        inline_cell(138,uc::primary_prompt(assignments,{uc::Action::weapon_previous,uc::Action::weapon_next}),150);
        inline_cell(139,uc::primary_prompt(assignments,{uc::Action::reload}),150);
        const auto sculpted_key=renderer.capture(139);
        renderer.set_prompt_style(uc::PromptStyle::monochrome);
        inline_cell(140,uc::primary_prompt(assignments,{uc::Action::reload}),150);
        CHECK(renderer.capture(140).rgba!=sculpted_key.rgba);
        renderer.set_prompt_style(uc::PromptStyle::sculpted);
        work.serial=141;
        gx::PromptDraw main_glyph{uc::primary_prompt(assignments,{uc::Action::interact}),{300,200,324,200,324,226,300,226}};main_glyph.compact=true;
        auto shadow=main_glyph;for(auto& tint:shadow.tint)tint={0,0,0};
        for(size_t i=0;i<4;++i) {shadow.efb_corners[i*2]+=1;shadow.efb_corners[i*2+1]+=.75f;}
        work.prompts={shadow};submit(work);sampled=renderer.capture(141);
        CHECK(active_pixels(sampled)==0);
        size_t shadow_pixels=0;for(size_t i=0;i<sampled.rgba.size();i+=4)if(sampled.rgba[i]==0 && sampled.rgba[i+1]==0 && sampled.rgba[i+2]==0)++shadow_pixels;
        CHECK(shadow_pixels>150);
        work.serial=142;work.prompts={shadow,main_glyph};submit(work);sampled=renderer.capture(142);
        auto white_pixels=[](const uc::RenderPixels& pixels) {size_t count=0;for(size_t i=0;i<pixels.rgba.size();i+=4)if(pixels.rgba[i]==255 && pixels.rgba[i+1]==255 && pixels.rgba[i+2]==255)++count;return count;};
        CHECK(white_pixels(sampled)<=white_pixels(sculpted_left)+2 && active_pixels(sampled)>20);
        uc::save_png(sampled,executable_dir()/"native-prompt-shadow.png");
        bool invalid_scale=false; try { renderer.set_render_scale(7); } catch(const std::invalid_argument&) { invalid_scale=true; }
        CHECK(invalid_scale);
        renderer.set_render_scale(0.5);
        std::this_thread::sleep_for(uc::kRebuildInterval);
        packet.serial = 143;packet.prompts=work.prompts;draw.indices[2] = 999;
        try { renderer.render(packet); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected); draw.indices[2] = 2;
        std::atomic<bool> wrong_owner_rejected{false};
        std::thread outsider([&] { try { renderer.resize(1, 1); } catch (const std::logic_error&) { wrong_owner_rejected = true; } });
        outsider.join(); CHECK(wrong_owner_rejected);
        auto end = std::chrono::steady_clock::now() + 3s;
        while (!renderer.render(packet)) { CHECK(std::chrono::steady_clock::now() < end); std::this_thread::sleep_for(1ms); }
        renderer.shutdown();
        CHECK(renderer.poll_completions() == std::vector<uint64_t>({143}));
        CHECK(renderer.validation_errors() == 0);
        uc::VulkanRenderer portrait;
        portrait.initialize(window,validation,executable_dir()/"data"/"shaders",0.5,0.25);
        auto thin_copy=std::make_shared<gx::TexturePixels>(); thin_copy->gpu_only=true;
        for(uint32_t width=1024;;width/=2) { thin_copy->levels.push_back({width,1,{}}); if(width==1) break; }
        work=packet; work.serial=1; work.present=false; work.clear_after_copy=false; work.draws[0]=quad;
        work.texture_copy=gx::TextureCopy{{0,0,640,480},thin_copy}; submit(work,&portrait);
        work.serial=2; work.present=true; work.texture_copy.reset(); work.draws[0].texture=thin_copy;
        for(auto& vertex:work.draws[0].vertices) { vertex.color[0]={255,255,255,255}; vertex.texcoord[0]={.5f,.5f}; }
        submit(work,&portrait); sampled=portrait.capture(2);
        center=(size_t(sampled.height/2)*sampled.width+sampled.width/2)*4;
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0);
        // GX fog: orthographic linear fog offset by C=-1 saturates to the BP fog colour.
        work.serial=3; work.texture_copy.reset(); work.draws[0]=quad;
        for(auto& vertex:work.draws[0].vertices) vertex.color[0]={20,200,20,255};
        work.draws[0].uniforms.edit().fog={0,0,0,(2u<<21)|(1u<<20)|(1u<<19)|(127u<<11),0xFF0000}; // C=-1: fog 1 at any depth
        submit(work,&portrait); sampled=portrait.capture(3);
        center=(size_t(sampled.height/2)*sampled.width+sampled.width/2)*4;
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0 && sampled.rgba[center+2]==0);
        work.serial=4; work.draws[0].uniforms.edit().fog[3]=0; submit(work,&portrait); sampled=portrait.capture(4);
        CHECK(sampled.rgba[center]==20 && sampled.rgba[center+1]==200); // fsel 0 leaves the TEV colour
        // GX indirect texturing: static matrix 0 (ma~1, scale 2^-7) turns an alpha offset of 127 into ~1 texel in S.
        auto two_texels=std::make_shared<gx::TexturePixels>(); two_texels->levels={{2,1,{255,0,0,255,0,0,255,255}}};
        auto offsets=std::make_shared<gx::TexturePixels>(); offsets->levels={{1,1,{128,128,128,255}}};
        work.serial=5; work.draws[0]=quad; work.draws[0].texture=two_texels; work.draws[0].texture_maps[0]=offsets;
        for(auto& vertex:work.draws[0].vertices) { vertex.color[0]={255,255,255,255}; vertex.texcoord[0]={0.25f,0.5f}; }
        work.draws[0].material.stages=1; work.draws[0].material.swaps={0xE4,0xE4,0xE4,0xE4};
        work.draws[0].material.stage[0]={0x8FFF8,0x8FF80,0x40,0,0x210};
        work.draws[0].uniforms.edit().indirect={0x8003FF,0x800000,0,0,0,0,0,0,0,1,0,0};
        work.draws[0].uniforms.edit().ind_cmd[0]=0x210;
        submit(work,&portrait); sampled=portrait.capture(5);
        CHECK(sampled.rgba[center]==0 && sampled.rgba[center+2]==255);
        work.serial=6; work.draws[0].material.stage[0].indirect=0; work.draws[0].uniforms.edit().ind_cmd[0]=0;
        submit(work,&portrait); sampled=portrait.capture(6);
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+2]==0); // without the indirect stage the left texel stays
        // Texgen STQ from the position (ABC1): left S/Q 0.8/1, right 0.6/3.
        work.serial=7;
        work.draws[0].uniforms.edit().tex_gens[0]=6;
        work.draws[0].uniforms.edit().tex_rows[0]={-0.1f,0,0,0.7f}; work.draws[0].uniforms.edit().tex_rows[1]={0.5f,0,0,1}; work.draws[0].uniforms.edit().tex_rows[2]={1,0,0,2};
        submit(work,&portrait);sampled=portrait.capture(7);
        const auto projected_sample=(size_t(sampled.height/2)*sampled.width+sampled.width*2/5)*4;
        // Divide interpolated ST by interpolated Q; dividing at vertices would select blue.
        CHECK(sampled.rgba[projected_sample]==255 && sampled.rgba[projected_sample+2]==0);
        // Texgen ST from the untransformed normal (1,0,0), normalized (S=0.707) before post S'=1.25-S: right texel. Without normalizing S'=0.25.
        work.serial=8;
        work.draws[0].uniforms.edit().tex_gens[0]=(1u<<7)|4; work.draws[0].uniforms.edit().tex_posts[0]=256;
        work.draws[0].uniforms.edit().tex_rows[0]={1,0,0,0}; work.draws[0].uniforms.edit().tex_rows[1]={0,1,0,0}; work.draws[0].uniforms.edit().tex_rows[2]={0,0,1,0};
        work.draws[0].uniforms.edit().post_rows[0]={-1,0,0,1.25f}; work.draws[0].uniforms.edit().post_rows[1]={0,0,0,0.5f}; work.draws[0].uniforms.edit().post_rows[2]={0,0,0,1};
        for(auto& vertex:work.draws[0].vertices) vertex.normal={1,0,0};
        submit(work,&portrait);sampled=portrait.capture(8);
        CHECK(sampled.rgba[center]==0 && sampled.rgba[center+2]==255);
        // Integer TEV: input A uses only the low 8 bits of an s10 register, 1.5 (382) reads as 126.
        work.serial=9; work.draws[0].uniforms=gx::DrawUniform{};
        work.draws[0].material.stages=1; work.draws[0].material.stage[0]={0x82FFF,0x8FF80,0x380,0,0};
        work.draws[0].uniforms.edit().registers[1]={1.5f,1.5f,1.5f,1};
        submit(work,&portrait);sampled=portrait.capture(9);
        CHECK(sampled.rgba[center]==126 && sampled.rgba[center+1]==126);
        // Blanked VI output: the presented frame is black although the EFB holds the same draw.
        work.serial=10; work.black_output=true;
        submit(work,&portrait);sampled=portrait.capture(10);
        CHECK(sampled.rgba[center]==0 && sampled.rgba[center+1]==0 && sampled.rgba[center+2]==0);
        work.black_output=false;
        // Per-vertex texture matrix id 5 (S += 0.5, bank rows 15..17) replaces texgen 0's own identity matrix: S 0.25 -> 0.75 picks the right texel.
        work.serial=11; work.draws[0].uniforms=gx::DrawUniform{}; work.draws[0].material.stage[0]={0x8FFF8,0x8FF80,0x40,0,0};
        work.draws[0].state.edit().descriptors[1]=gx::Mode::direct;
        auto bank=std::make_shared<gx::TextureBank>(); bank->rows[15]={1,0,0,0.5f};
        work.draws[0].texture_bank=bank;
        for(auto& vertex:work.draws[0].vertices) vertex.texture_matrix[0]=5;
        submit(work,&portrait);sampled=portrait.capture(11);
        CHECK(sampled.rgba[center]==0 && sampled.rgba[center+2]==255);
        work.serial=12; for(auto& vertex:work.draws[0].vertices) vertex.texture_matrix[0]=255;   // no index for this vertex: the texgen keeps its own matrix
        submit(work,&portrait);sampled=portrait.capture(12);
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+2]==0);
        work.draws[0].state.edit().descriptors[1]=gx::Mode::none; work.draws[0].texture_bank=nullptr;
        // Reused slots must rewrite textures and uniforms after descriptor growth and shrinkage.
        uint64_t descriptor_serial=13;
        for(size_t count:{32u,33u,65u,2u,65u}) for(size_t repeat=0;repeat<6;++repeat) {
            work=packet; work.serial=descriptor_serial++; work.draws.assign(count,quad);
            const gx::Shared<gx::DrawUniform> shared_uniforms;
            for(auto& item:work.draws) {
                item.material=packet.draws[0].material;
                item.uniforms=shared_uniforms;
                item.texture=two_texels;
                for(auto& vertex:item.vertices) {vertex.color[0]={255,255,255,255};vertex.texcoord[0]={.75f,.5f};}
            }
            work.draws.back().texture=offsets;
            submit(work,&portrait); sampled=portrait.capture(work.serial);
            CHECK(sampled.rgba[center]==128 && sampled.rgba[center+1]==128 && sampled.rgba[center+2]==128);
            work.serial=descriptor_serial++;
            work.draws.back().material.stages=1;
            work.draws.back().material.stage[0]={0x82FFF,0x8FF80,0x380,0,0};
            submit(work,&portrait); sampled=portrait.capture(work.serial);
            CHECK(sampled.rgba[center]==0 && sampled.rgba[center+1]==0 && sampled.rgba[center+2]==0);
            work.draws.back().material=packet.draws[0].material;
            work.serial=descriptor_serial++;work.draws.back().uniforms.edit().pixel[0]=0;
            submit(work,&portrait); sampled=portrait.capture(work.serial);
            CHECK(sampled.rgba[center]==0 && sampled.rgba[center+2]==255);
        }
        // In-game menu: drawn over the presented frame while set, nothing at all once cleared.
        work=packet; work.serial=descriptor_serial++; submit(work,&portrait); const auto plain=portrait.capture(work.serial);
        portrait.set_menu([] { ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({120,80}); ImGui::Begin("menu"); ImGui::TextUnformatted("menu"); ImGui::End(); });
        work.serial=descriptor_serial++; submit(work,&portrait); CHECK(portrait.capture(work.serial).rgba!=plain.rgba);
        portrait.set_menu({});
        work.serial=descriptor_serial++; submit(work,&portrait); CHECK(portrait.capture(work.serial).rgba==plain.rgba);
        // A renderer that replaced another draws the EFB copies the old one held (lost_copies) blank instead of failing.
        auto lost=std::make_shared<gx::TexturePixels>(); lost->gpu_only=true; lost->levels={{4,4,{}}};
        work.serial=descriptor_serial++; work.draws[0].texture=lost;
        bool unknown_copy=false;
        try { portrait.render(work); } catch(const std::invalid_argument&) { unknown_copy=true; }
        CHECK(unknown_copy);
        portrait.lost_copies({lost.get()}); submit(work,&portrait);
        portrait.shutdown(); CHECK(portrait.validation_errors()==0);
        SDL_DestroyWindow(window); window = nullptr; SDL_Quit();
        std::printf("Vulkan smoke passed: triangle pixels, resize, zero extent, completion once, clean shutdown\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Vulkan smoke failed: %s\n", error.what());
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
}
