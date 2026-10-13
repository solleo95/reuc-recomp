#include "check.h"
#include "vulkan_renderer.h"
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
        window = SDL_CreateWindow("Umbrella Chronicles — Vulkan smoke", 320, 240, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!window) throw std::runtime_error(SDL_GetError());
        std::fprintf(stderr,"GPU smoke: initialize renderer\n");
        uc::VulkanRenderer renderer;
        renderer.initialize(window, validation, executable_dir() / "data" / "shaders");
        gx::FramePacket packet;
        packet.draws.emplace_back();
        auto& draw = packet.draws.back(); draw.primitive = 0x90;
        draw.vertices.resize(3); draw.indices = {0,1,2};
        draw.vertices[0].position = {-8,-8,5}; draw.vertices[0].color[0] = {255,0,0,255};
        draw.vertices[1].position = {12,-8,5}; draw.vertices[1].color[0] = {0,255,0,255};
        draw.vertices[2].position = {0,8,5}; draw.vertices[2].color[0] = {0,0,255,255};
        draw.state.descriptors[0]=gx::Mode::direct;
        draw.vertices[1].position_matrix=3;
        gx::set_position_matrix(draw.transforms,0,{{{0.2f,0,0,0},{0,0.2f,0,0},{0,0,-0.4f,0}}});
        gx::set_position_matrix(draw.transforms,3,{{{0.2f,0,0,-0.8f},{0,0.2f,0,0},{0,0,-0.4f,0}}});
        draw.transforms.current=9; // per-vertex indices override the unloaded current matrix
        draw.transforms.projection=gx::make_projection({1,0,-1,0,-1.f/9,-10.f/9},0);
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
        CHECK(SDL_SetWindowSize(window, 640, 360));
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
        draw.transforms.position_rows[2]=draw.transforms.position_rows[5]={0,0,0,-0.5f};
        draw.transforms.projection=gx::make_projection({0.5f,0,-0.5f,0,1,0},1);
        // Both GPU slots stay active; consumer completion order follows native submission serials.
        uint64_t expected = 4;
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
        auto submit=[&](gx::FramePacket& work) {
            auto until=std::chrono::steady_clock::now()+3s;
            bool sent=false,done=false;
            while (!done) {
                CHECK(std::chrono::steady_clock::now()<until);
                if (!sent) sent=renderer.render(work);
                for (auto serial : renderer.poll_completions()) { CHECK(serial==work.serial && !done); done=true; }
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
        CHECK(SDL_SetWindowSize(window,960,540));
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
        // A replacement image must reach both alternating GPU slots.
        const auto texture_creations=renderer.texture_creations();
        auto changed=std::make_shared<gx::TexturePixels>(*texture);
        changed->levels[0].rgba[12]=224;
        work.draws[0].texture=changed; work.serial=27; submit(work);
        sampled=renderer.capture(27); CHECK(sampled.rgba[center]==224);
        const auto after_changed=renderer.texture_creations(); CHECK(after_changed>=texture_creations+1);
        work.draws[0].texture=texture; work.serial=28; submit(work);
        sampled=renderer.capture(28); CHECK(sampled.rgba[center]==32);
        CHECK(renderer.texture_creations()==after_changed); // unchanged slot26 versions are reused in slot28
        for (auto& vertex : work.draws[0].vertices) vertex.texcoord[0]={0.25f,0.25f};
        work.serial=29; submit(work);
        sampled=renderer.capture(29); CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0 && sampled.rgba[center+2]==0);
        auto invalid=std::make_shared<gx::TexturePixels>(*texture); invalid->levels[0].rgba.pop_back();
        work.draws[0].texture=invalid; work.serial=30;
        bool invalid_texture=false;
        try { renderer.render(work); } catch (const std::invalid_argument&) { invalid_texture=true; }
        CHECK(invalid_texture);
        invalid=std::make_shared<gx::TexturePixels>(*texture); invalid->levels.push_back({2,1,{255,255,255,255,255,255,255,255}});
        work.draws[0].texture=invalid; invalid_texture=false;
        try { renderer.render(work); } catch (const std::invalid_argument&) { invalid_texture=true; }
        CHECK(invalid_texture);
        work=packet; work.serial=30; work.draws[0].texture.reset();
        work.draws[0].blend={1,4,5,3,true,true};
        for (auto& vertex:work.draws[0].vertices) vertex.color[0]={255,0,0,0};
        submit(work); sampled=renderer.capture(30);
        CHECK(sampled.rgba[center]==16 && sampled.rgba[center+1]==32 && sampled.rgba[center+2]==64);
        work.serial=31;
        for (auto& vertex:work.draws[0].vertices) vertex.color[0]={255,0,0,128};
        submit(work); sampled=renderer.capture(31);
        CHECK(sampled.rgba[center]>=134 && sampled.rgba[center]<=137 && sampled.rgba[center+1]>=15 && sampled.rgba[center+1]<=17);
        work=packet; work.serial=32;
        auto red=std::make_shared<gx::TexturePixels>(); red->levels.push_back({1,1,{255,0,0,255}});
        auto mask=std::make_shared<gx::TexturePixels>(); mask->levels.push_back({1,1,{0,255,0,64}});
        work.draws[0].texture=red; work.draws[0].texture_maps[0]=mask;
        auto& material=work.draws[0].material; material.stages=2; material.swaps={0xE4,0xE4,0xE4,0xE4};
        material.stage[0]={0x8FFF8,0x8FFC0,0x3C0,0,0};
        material.stage[1]={0x8FFF0,0x8FFC0,0x3C1,0,0};
        submit(work); sampled=renderer.capture(32);
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0 && sampled.rgba[center+2]==0 && sampled.rgba[center+3]==64);
        work.serial=33; submit(work); // populate both GPU slots
        const auto before_copy_only=renderer.texture_creations();
        auto copy_only=work;
        // A nonempty postprocessing/UI batch must also retain the scene's immutable textures.
        copy_only.draws[0].texture.reset(); copy_only.draws[0].texture_maps={}; copy_only.draws[0].material.stages=0;
        copy_only.serial=34; submit(copy_only);
        copy_only.serial=35; submit(copy_only);
        work.serial=36; submit(work);
        CHECK(renderer.texture_creations()==before_copy_only);
        sampled=renderer.capture(36);
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+3]==64);
        work.serial=37; work.draws[0].uniforms.pixel[0]=0; submit(work);
        sampled=renderer.capture(37); CHECK(sampled.rgba[center]==16 && sampled.rgba[center+1]==32);
        work.serial=38; work.draws[0].uniforms.pixel={0x3F0000,0,256|123,2}; submit(work);
        sampled=renderer.capture(38); CHECK(sampled.rgba[center]==255 && sampled.rgba[center+3]==123);
        work.draws[0].uniforms.pixel={0x3F0000,31,0,2};
        auto hidden=work.draws[0]; hidden.material.stages=0; hidden.texture.reset(); hidden.uniforms.pixel[1]=17;
        for(auto& vertex:hidden.vertices) vertex.color[0]={0,255,0,255};
        work.draws.push_back(hidden); work.serial=39; submit(work);
        sampled=renderer.capture(39); CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0 && sampled.rgba[center+3]==64);
        work.draws.resize(1); work.serial=40;
        work.draws[0].blend={1,4,5,3,true,true}; work.draws[0].uniforms.pixel={0x3F0000,0,256|123,2};
        submit(work); sampled=renderer.capture(40);
        CHECK(sampled.rgba[center]>=75 && sampled.rgba[center]<=77 && sampled.rgba[center+1]>=23 && sampled.rgba[center+1]<=25 && sampled.rgba[center+3]==123);
        work=packet; work.serial=41;
        auto mips=std::make_shared<gx::TexturePixels>();
        mips->levels.push_back({2,2,{255,0,0,255,255,0,0,255,255,0,0,255,255,0,0,255}});
        mips->levels.push_back({1,1,{17,33,65,255}}); work.draws[0].texture=mips;
        for(auto& vertex:work.draws[0].vertices) vertex.color[0]={255,255,255,255};
        work.draws[0].vertices[0].texcoord[0]={0,0}; work.draws[0].vertices[1].texcoord[0]={512,0}; work.draws[0].vertices[2].texcoord[0]={0,512};
        work.draws[0].texture_wrap[0]=0x20; // Explicit nearest mip selection.
        submit(work); sampled=renderer.capture(41);
        CHECK(sampled.rgba[center]==17 && sampled.rgba[center+1]==33 && sampled.rgba[center+2]==65);
        auto old_copy=std::make_shared<gx::TexturePixels>(); old_copy->gpu_only=true;
        old_copy->levels={{4,4,{}},{2,2,{}},{1,1,{}}};
        auto new_copy=std::make_shared<gx::TexturePixels>(*old_copy);
        work=packet; work.serial=42; work.present=false; work.clear_color={0,255,0,255};
        auto quad=work.draws[0]; quad.material.stages=0; quad.primitive=0x90;
        quad.transforms={}; quad.state.descriptors[0]=gx::Mode::none;
        quad.vertices.resize(4); quad.indices={0,1,2,0,2,3};
        for(auto& vertex:quad.vertices) vertex.color[0]={255,0,0,255};
        quad.vertices[0].position={-1,-1,0}; quad.vertices[1].position={1,-1,0};
        quad.vertices[2].position={1,1,0}; quad.vertices[3].position={-1,1,0};
        work.draws[0]=quad;
        work.texture_copy=gx::TextureCopy{{0,0,640,480},old_copy}; submit(work);
        work.serial=43; work.draws.clear(); work.clear_color={0,0,255,255};
        work.texture_copy=gx::TextureCopy{{0,0,640,480},new_copy}; submit(work);
        work=packet; work.serial=44; work.draws[0].texture=old_copy;
        for(auto& vertex:work.draws[0].vertices) { vertex.color[0]={255,255,255,255}; vertex.texcoord[0]={0.5f,0.5f}; }
        work.draws[0].vertices[0].texcoord[0]={0,0}; work.draws[0].vertices[1].texcoord[0]={512,0}; work.draws[0].vertices[2].texcoord[0]={0,512};
        submit(work); sampled=renderer.capture(44);
        std::fprintf(stderr,"EFB old-copy pixel %u %u %u %u\n",sampled.rgba[center],sampled.rgba[center+1],sampled.rgba[center+2],sampled.rgba[center+3]);
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0);
        work.serial=45; work.draws[0].texture=new_copy; submit(work); sampled=renderer.capture(45);
        CHECK(sampled.rgba[center]==0 && sampled.rgba[center+1]==255);
        CHECK(SDL_SetWindowSize(window,1024,576)); renderer.resize(1024,576);
        work.serial=46; work.draws[0].texture=old_copy; submit(work); sampled=renderer.capture(46);
        center=(size_t(sampled.height/2)*sampled.width+sampled.width/2)*4;
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0);
        auto ephemeral=std::make_shared<gx::TexturePixels>(); ephemeral->levels={{1,1,{17,33,65,255}}};
        std::weak_ptr<const gx::TexturePixels> released=ephemeral;
        work.serial=47; work.draws[0].texture=ephemeral; submit(work);
        work.serial=48; submit(work); // Both slots own this version.
        ephemeral.reset(); work.draws[0].texture.reset();
        work.serial=49; submit(work); CHECK(!released.expired());
        work.serial=50; submit(work); CHECK(released.expired());
        work=packet; work.serial=51;
        work.draws[0]=quad; // A full-screen red quad, limited by native viewport and scissor.
        work.draws[0].state.viewport=std::array<float,6>{0,0,320,480,0,1};
        work.draws[0].state.scissor=std::array<int32_t,4>{0,0,160,480};
        submit(work); sampled=renderer.capture(51);
        const auto inside=(size_t(sampled.height/2)*sampled.width+sampled.width/8)*4;
        const auto clipped=(size_t(sampled.height/2)*sampled.width+sampled.width*3/8)*4;
        CHECK(sampled.rgba[inside]==255 && sampled.rgba[inside+1]==0);
        CHECK(sampled.rgba[clipped]==16 && sampled.rgba[clipped+1]==32);
        bool rejected = false;
        work=packet; work.serial=52; work.draws[0]=quad;
        work.draws[0].state.viewport=std::array<float,6>{0,0,640,360,0,1};
        work.display_source=std::array<uint32_t,4>{0,0,640,360};
        submit(work); sampled=renderer.capture(52);
        const auto bottom=(size_t(sampled.height*7/8)*sampled.width+sampled.width/2)*4;
        CHECK(sampled.rgba[bottom]==255 && sampled.rgba[bottom+1]==0);
        auto wrapped=std::make_shared<gx::TexturePixels>(); wrapped->levels={{2,1,{255,0,0,255,0,0,255,255}}};
        work=packet; work.serial=53; work.draws[0]=quad; work.draws[0].texture=wrapped;
        for(auto& vertex:work.draws[0].vertices) { vertex.color[0]={255,255,255,255}; vertex.texcoord[0]={1.1f,0.5f}; }
        submit(work); sampled=renderer.capture(53); CHECK(sampled.rgba[center]==0 && sampled.rgba[center+2]==255);
        work.serial=54; work.draws[0].texture_wrap[0]=1;
        submit(work); sampled=renderer.capture(54); CHECK(sampled.rgba[center]==255 && sampled.rgba[center+2]==0);
        work.serial=55; work.draws[0].texture_wrap[0]=2;
        submit(work); sampled=renderer.capture(55); CHECK(sampled.rgba[center]==0 && sampled.rgba[center+2]==255);
        work.serial=56; work.draws[0].texture_maps[0]=wrapped;
        work.draws[0].material.stages=1; work.draws[0].material.swaps={0xE4,0xE4,0xE4,0xE4};
        work.draws[0].material.stage[0]={0x8FFF8,0x8FF80,0x41,0,0};
        work.draws[0].texture_wrap[0]=0; work.draws[0].texture_wrap[1]=1;
        submit(work); sampled=renderer.capture(56); CHECK(sampled.rgba[center]==255 && sampled.rgba[center+2]==0);
        work.serial=57; work.draws[0].texture_wrap[1]=0;
        submit(work); sampled=renderer.capture(57); CHECK(sampled.rgba[center]==0 && sampled.rgba[center+2]==255);
        auto partial_copy=std::make_shared<gx::TexturePixels>(); partial_copy->gpu_only=true; partial_copy->levels={{4,4,{}}};
        work=packet; work.serial=58; work.present=false; work.draws[0]=quad; work.clear_color={0,255,0,255};
        work.texture_copy=gx::TextureCopy{{0,0,320,480},partial_copy}; submit(work);
        auto intermediate=renderer.capture(58,true);
        const auto efb_left=(size_t(intermediate.height/2)*intermediate.width+intermediate.width/4)*4;
        const auto efb_right=(size_t(intermediate.height/2)*intermediate.width+intermediate.width*3/4)*4;
        CHECK(intermediate.rgba[efb_left]==0 && intermediate.rgba[efb_left+1]==255);
        CHECK(intermediate.rgba[efb_right]==255 && intermediate.rgba[efb_right+1]==0);
        work=packet; work.serial=59; work.draws.clear(); work.clear_after_copy=false;
        submit(work); sampled=renderer.capture(59);
        bool stale_efb=false;
        try { renderer.capture(58,true); } catch(const std::invalid_argument&) { stale_efb=true; }
        CHECK(stale_efb);
        const auto left=(size_t(sampled.height/2)*sampled.width+sampled.width/4)*4;
        const auto right=(size_t(sampled.height/2)*sampled.width+sampled.width*3/4)*4;
        CHECK(sampled.rgba[left]==0 && sampled.rgba[left+1]==255);
        CHECK(sampled.rgba[right]==255 && sampled.rgba[right+1]==0);
        work=packet; work.serial=60; work.draws[0]=quad; work.draws[0].texture=wrapped;
        for(auto& vertex:work.draws[0].vertices) { vertex.color[0]={255,255,255,255}; vertex.texcoord[0]={0.1f,0.5f}; }
        work.draws[0].uniforms.tex_rows[0]={1,0,0.75f,0}; // AB11 uses both homogeneous components
        submit(work); sampled=renderer.capture(60);
        CHECK(sampled.rgba[center]==0 && sampled.rgba[center+2]==255);
        work=packet; work.serial=61; work.draws[0]=quad;
        work.draws[0].uniforms.channels={2,1,0,1};
        work.draws[0].uniforms.materials[0]={1,1,1,1};
        work.draws[0].uniforms.ambient[0]={9/255.f,9/255.f,9/255.f,1};
        submit(work); sampled=renderer.capture(61);
        std::fprintf(stderr,"native ambient pixel %u %u %u %u\n",sampled.rgba[center],sampled.rgba[center+1],sampled.rgba[center+2],sampled.rgba[center+3]);
        CHECK(sampled.rgba[center]==8 && sampled.rgba[center+1]==8 && sampled.rgba[center+2]==8); // 255*9 >> 8
        work.serial=62; work.draws[0].uniforms.channels[0]=6;
        work.draws[0].uniforms.lights[0].color={128,128,128,255};
        work.draws[0].uniforms.lights[0].position={0,0,1,0};
        submit(work); sampled=renderer.capture(62);
        CHECK(sampled.rgba[center]==137 && sampled.rgba[center+1]==137);
        work=packet; work.serial=63; work.draws[0]=quad; work.draws[0].state.cull=3;
        submit(work); sampled=renderer.capture(63);
        CHECK(sampled.rgba[center]==16 && sampled.rgba[center+1]==32 && sampled.rgba[center+2]==64);
        work=packet; work.serial=64; work.draws[0]=quad; work.draws[0].uniforms.pixel[3]=0;
        submit(work); sampled=renderer.capture(64);
        CHECK(sampled.rgba[center]==0 && sampled.rgba[center+1]==0 && sampled.rgba[center+2]==0);
        work=packet; work.serial=65; work.draws[0]=quad; work.draws[0].texture=wrapped;
        for(auto& vertex:work.draws[0].vertices) { vertex.color[0]={255,255,255,255}; vertex.texcoord[0]={0.5f,0.5f}; }
        work.draws[0].texture_wrap[0]=0x10; // Linear magnification blends the two texels.
        submit(work); sampled=renderer.capture(65);
        CHECK(sampled.rgba[center]>=127 && sampled.rgba[center]<=128 && sampled.rgba[center+2]>=127 && sampled.rgba[center+2]<=128);
        work=packet; work.serial=66; work.draws[0]=quad; work.draws[0].texture=mips;
        for(auto& vertex:work.draws[0].vertices) vertex.color[0]={255,255,255,255};
        work.draws[0].vertices[0].texcoord[0]={0,0}; work.draws[0].vertices[1].texcoord[0]={512,0};
        work.draws[0].vertices[2].texcoord[0]={512,512}; work.draws[0].vertices[3].texcoord[0]={0,512};
        submit(work); sampled=renderer.capture(66);
        CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0 && sampled.rgba[center+2]==0); // Mips exist but this sampler disables them.
        packet.serial = 67; draw.indices[2] = 999;
        try { renderer.render(packet); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected); draw.indices[2] = 2;
        std::atomic<bool> wrong_owner_rejected{false};
        std::thread outsider([&] { try { renderer.resize(1, 1); } catch (const std::logic_error&) { wrong_owner_rejected = true; } });
        outsider.join(); CHECK(wrong_owner_rejected);
        auto end = std::chrono::steady_clock::now() + 3s;
        while (!renderer.render(packet)) { CHECK(std::chrono::steady_clock::now() < end); std::this_thread::sleep_for(1ms); }
        renderer.shutdown();
        CHECK(renderer.poll_completions() == std::vector<uint64_t>({67}));
        CHECK(renderer.validation_errors() == 0);
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
