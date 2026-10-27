#include "check.h"
#include "rt_ui.h"
#include "backend_ui.h"
int main() {
    uc::GraphicsEffects effects;
    // The report has not arrived yet: nothing is blocked, the checkbox is usable, the quality waits for it
    auto ui=uc::rt_ui(effects,false,false);
    CHECK(ui.master_enabled && !ui.quality_enabled && !ui.raster_replaced);
    // A GPU without ray tracing: the group is greyed; a saved "on" stays visible and can be switched off, the quality cannot be changed and nothing is replaced (the renderer masks it off)
    ui=uc::rt_ui(effects,true,false);
    CHECK(!ui.master_enabled && !ui.quality_enabled && !ui.raster_replaced);
    effects.path_tracing=true;
    ui=uc::rt_ui(effects,true,false);
    CHECK(ui.master_enabled && !ui.quality_enabled && !ui.raster_replaced);
    // A supported GPU: the checkbox enables the quality; path tracing lights the scene, so the shadow map, the contact shadows and GTAO are greyed
    effects=uc::GraphicsEffects{};
    ui=uc::rt_ui(effects,true,true);
    CHECK(ui.master_enabled && !ui.quality_enabled && !ui.raster_replaced);
    effects.path_tracing=true;
    ui=uc::rt_ui(effects,true,true);
    CHECK(ui.master_enabled && ui.quality_enabled && ui.raster_replaced);
    effects.path_tracing=false;   // off with the quality kept: the raster options are back
    ui=uc::rt_ui(effects,true,true);
    CHECK(ui.master_enabled && !ui.quality_enabled && !ui.raster_replaced);
    // the legacy master switch and levels are not the player's options: they change nothing here
    effects=uc::GraphicsEffects{}; effects.ray_tracing=true; effects.rt_shadows=uc::EffectLevel::high; effects.rt_ambient_occlusion=uc::EffectLevel::high;
    ui=uc::rt_ui(effects,true,true);
    CHECK(!ui.quality_enabled && !ui.raster_replaced);
    // The graphics API decides what the launcher and the menu offer at all (DirectX 11: no DXR, no FSR, no frame generation, no DLSS 5)
    for(const auto api:{uc::RendererBackend::vulkan,uc::RendererBackend::directx12}) {
        const auto offered=uc::backend_ui(api);
        CHECK(offered.ray_tracing && offered.fsr && offered.frame_generation && offered.dlss5);
    }
    const auto dx11=uc::backend_ui(uc::RendererBackend::directx11);
    CHECK(!dx11.ray_tracing && !dx11.fsr && !dx11.frame_generation && !dx11.dlss5);
    return 0;
}
