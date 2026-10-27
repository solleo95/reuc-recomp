#include "check.h"
#include "game_renderer.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <filesystem>

// DirectX 11 has no ray tracing, FSR or frame generation. A saved or live choice of those must be dropped by the renderer, not throw
// (path tracing switched on in the menu before the feature report arrived used to end the game).
int main() {
    SDL_Window* window=nullptr;
    try {
        CHECK(SDL_Init(SDL_INIT_VIDEO));
        window=SDL_CreateWindow("DirectX 11 option limits",1280,720,SDL_WINDOW_HIDDEN);CHECK(window);
        const auto directory=std::filesystem::path(SDL_GetBasePath());
        uc::GraphicsEffects effects;effects.path_tracing=effects.ray_tracing=effects.ray_reconstruction=true;effects.rt_shadows=uc::EffectLevel::high;
        uc::GameRenderer renderer(uc::RendererBackend::directx11);
        renderer.configure_upscaling(uc::UpscalerKind::fsr,uc::UpscaleMode::quality,false,.5,true,2);   // as saved by a Vulkan or DirectX 12 session
        renderer.set_effects(effects);
        renderer.initialize(window,true,directory/"data/shaders",1,16./9,false);
        CHECK(renderer.active_upscaler()==uc::UpscalerKind::off && !renderer.frame_generation_active());
        renderer.set_effects(effects);   // live, as the menu does
        CHECK(renderer.set_upscaling(uc::UpscalerKind::fsr,uc::UpscaleMode::quality,false,.5,true,2));
        CHECK(renderer.active_upscaler()==uc::UpscalerKind::off && !renderer.frame_generation_active());
        gx::FramePacket packet;packet.present=true;packet.display_source={0,0,640,480};packet.serial=1;
        bool sent=false;for(int i=0;i<1000 && !sent;++i)sent=renderer.render(packet);
        CHECK(sent && renderer.validation_errors()==0);
        renderer.shutdown();
        SDL_DestroyWindow(window);SDL_Quit();std::puts("DirectX 11 ray tracing, FSR and frame generation choices are dropped without errors");return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"native game renderer smoke: %s\n",e.what());if(window)SDL_DestroyWindow(window);SDL_Quit();return 1;}
}
