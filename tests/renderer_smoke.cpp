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
        bool invalid_pixels_rejected=false;
        try { uc::save_png({},{}); } catch (const std::invalid_argument&) { invalid_pixels_rejected=true; }
        CHECK(invalid_pixels_rejected);
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
        window = SDL_CreateWindow("Umbrella Chronicles — Vulkan smoke", 320, 240, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!window) throw std::runtime_error(SDL_GetError());
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
                for (auto value : renderer.poll_completions()) { CHECK(value == serial); CHECK(!completed); completed = true; }
                std::this_thread::sleep_for(1ms);
            }
            CHECK(submitted && completed && renderer.poll_completions().empty());
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
        work.draws[0].texture=texture; work.serial=28; submit(work);
        sampled=renderer.capture(28); CHECK(sampled.rgba[center]==32);
        CHECK(renderer.texture_creations()==texture_creations+1); // unchanged slot26 texture is reused in slot28
        for (auto& vertex : work.draws[0].vertices) vertex.texcoord[0]={0.25f,0.25f};
        work.serial=29; submit(work);
        sampled=renderer.capture(29); CHECK(sampled.rgba[center]==255 && sampled.rgba[center+1]==0 && sampled.rgba[center+2]==0);
        auto invalid=std::make_shared<gx::TexturePixels>(*texture); invalid->levels[0].rgba.pop_back();
        work.draws[0].texture=invalid; work.serial=30;
        bool invalid_texture=false;
        try { renderer.render(work); } catch (const std::invalid_argument&) { invalid_texture=true; }
        CHECK(invalid_texture);
        invalid=std::make_shared<gx::TexturePixels>(*texture); invalid->levels.push_back({1,1,{255,255,255,255}});
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
        bool rejected = false;
        packet.serial = 32; draw.indices[2] = 999;
        try { renderer.render(packet); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected); draw.indices[2] = 2;
        std::atomic<bool> wrong_owner_rejected{false};
        std::thread outsider([&] { try { renderer.resize(1, 1); } catch (const std::logic_error&) { wrong_owner_rejected = true; } });
        outsider.join(); CHECK(wrong_owner_rejected);
        auto end = std::chrono::steady_clock::now() + 3s;
        while (!renderer.render(packet)) { CHECK(std::chrono::steady_clock::now() < end); std::this_thread::sleep_for(1ms); }
        renderer.shutdown();
        CHECK(renderer.poll_completions() == std::vector<uint64_t>({32}));
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
