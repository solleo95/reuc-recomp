#include "gx_stream.h"
#include "image_compare.h"
#include "vulkan_renderer.h"
#include <Windows.h>
#include <charconv>
#include <chrono>
#include <cstdio>

int main(int argc,char** argv) {
    SDL_Window* window=nullptr;
    try {
        if(argc<4)throw std::runtime_error("usage: uc_fifo_replay capture.dff frame output.png [--validation] [--reference image.png]");
        bool validation=false;std::filesystem::path reference_path;
        for(int i=4;i<argc;++i) {
            if(std::string_view(argv[i])=="--validation")validation=true;
            else if(std::string_view(argv[i])=="--reference" && i+1<argc)reference_path=argv[++i];
            else throw std::runtime_error("unknown or incomplete option");
        }
        std::optional<uc::RenderPixels> reference;
        auto output=std::filesystem::path(argv[3]);auto diff=output.parent_path()/(output.stem().string()+"-diff.png");
        if(!reference_path.empty()) {
            auto canonical=std::filesystem::weakly_canonical(reference_path);
            if(canonical==std::filesystem::weakly_canonical(output) || canonical==std::filesystem::weakly_canonical(diff) ||
               (std::filesystem::exists(output) && std::filesystem::equivalent(output,reference_path)) ||
               (std::filesystem::exists(diff) && std::filesystem::equivalent(diff,reference_path)))
                throw std::runtime_error("output must not overwrite reference image");
            reference=fifo_replay::load_reference(reference_path);
            if(reference->width!=640 || reference->height!=480)throw std::runtime_error("reference must be 640x480; automatic resizing would hide comparison errors");
        }
        size_t frame=0;std::string_view number=argv[2];auto parsed=std::from_chars(number.data(),number.data()+number.size(),frame);
        if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size())throw std::runtime_error("invalid frame number");
        auto capture=fifo_replay::load_dff(argv[1]);fifo_replay::GxStream stream(capture,frame);auto packets=stream.packets();
        std::fprintf(stderr,"replay: %zu packets, %llu draws, %llu vertices\n",packets.size(),stream.stats.draws,stream.stats.vertices);
        if(!SDL_Init(SDL_INIT_VIDEO))throw std::runtime_error(SDL_GetError());
        window=SDL_CreateWindow("Umbrella Chronicles — FIFO replay",640,480,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
        if(!window)throw std::runtime_error(SDL_GetError());
        auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
        if(!hwnd)throw std::runtime_error("replay window has no native handle");
        ShowWindow(hwnd,SW_SHOWNOACTIVATE);
        {
            uc::VulkanRenderer renderer;renderer.initialize(window,validation,std::filesystem::path(SDL_GetBasePath())/"data/shaders",2.0/3,4.0/3);
            uc::RenderPixels pixels;
            for(const auto& packet:packets) {
                bool submitted=false,completed=false;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
                while(!completed) {
                    if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("GPU replay timed out");
                    SDL_Event event;while(SDL_PollEvent(&event))if(event.type==SDL_EVENT_QUIT)throw std::runtime_error("replay canceled");
                    if(!submitted)submitted=renderer.render(packet);
                    if(submitted)renderer.wait_for_pending(1000000);
                    for(auto serial:renderer.poll_completions())if(serial==packet.serial)completed=true;
                    if(!completed)SDL_Delay(1);
                }
                if(packet.present)pixels=renderer.capture(packet.serial);
            }
            if(renderer.validation_errors())throw std::runtime_error("Vulkan validation errors");
            if(pixels.rgba.empty())throw std::runtime_error("capture has no displayed frame");
            uc::save_png(pixels,output);
            if(reference) {
                auto comparison=fifo_replay::compare_images(pixels,*reference);uc::save_png(comparison.diff,diff);
                std::fprintf(stderr,"RGB absolute difference: mean %.6f/255, peak %u/255, changed pixels %llu/%llu; %s\n",
                    comparison.mean,comparison.peak,comparison.changed_pixels,uint64_t(pixels.width)*pixels.height,diff.string().c_str());
            }
            renderer.shutdown();
        }
        SDL_DestroyWindow(window);SDL_Quit();std::fprintf(stderr,"saved %s\n",argv[3]);return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"replay: %s\n",e.what());if(window)SDL_DestroyWindow(window);SDL_Quit();return 1;
    }
}
