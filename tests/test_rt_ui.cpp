#include "check.h"
#include "rt_ui.h"
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
    return 0;
}
