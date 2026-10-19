#include "check.h"
#include "rt_ui.h"
int main() {
    using uc::EffectLevel;
    uc::GraphicsEffects effects;
    // The report has not arrived yet: nothing is blocked, the master checkbox is usable, the levels wait for it
    auto ui=uc::rt_ui(effects,false,false);
    CHECK(ui.master_enabled && !ui.levels_enabled && !ui.contact_replaced && !ui.shadows_replaced);
    // A GPU without ray tracing: the group is greyed; a saved "on" stays visible and can be switched off, the levels cannot be changed
    ui=uc::rt_ui(effects,true,false);
    CHECK(!ui.master_enabled && !ui.levels_enabled);
    effects.ray_tracing=true; effects.rt_shadows=EffectLevel::high;
    ui=uc::rt_ui(effects,true,false);
    CHECK(ui.master_enabled && !ui.levels_enabled && !ui.contact_replaced && !ui.shadows_replaced);   // nothing is replaced: the renderer masks RT off on this GPU
    // A supported GPU: the master enables the levels; RT shadows replace the shadow map and contact shadows, the other levels replace nothing
    effects=uc::GraphicsEffects{};
    ui=uc::rt_ui(effects,true,true);
    CHECK(ui.master_enabled && !ui.levels_enabled && !ui.contact_replaced);
    effects.ray_tracing=true;
    ui=uc::rt_ui(effects,true,true);
    CHECK(ui.master_enabled && ui.levels_enabled && !ui.contact_replaced && !ui.shadows_replaced);   // on, every level off: nothing active
    effects.rt_local_shadows=EffectLevel::low;
    ui=uc::rt_ui(effects,true,true);
    CHECK(!ui.contact_replaced && !ui.shadows_replaced);   // local shadows have no raster counterpart
    effects.rt_shadows=EffectLevel::medium;
    ui=uc::rt_ui(effects,true,true);
    CHECK(ui.contact_replaced && ui.shadows_replaced);
    effects.ray_tracing=false;   // the master off with the levels kept: the raster options are back
    ui=uc::rt_ui(effects,true,true);
    CHECK(!ui.contact_replaced && !ui.shadows_replaced && !ui.levels_enabled && ui.master_enabled);
    // RT ambient occlusion replaces GTAO (and only while the master is on on a GPU that can trace)
    effects=uc::GraphicsEffects{}; effects.ray_tracing=true; effects.rt_ambient_occlusion=EffectLevel::low;
    ui=uc::rt_ui(effects,true,true);
    CHECK(ui.ao_replaced && !ui.contact_replaced && !ui.shadows_replaced);
    effects.rt_ambient_occlusion=EffectLevel::off;
    CHECK(!uc::rt_ui(effects,true,true).ao_replaced);
    effects.rt_ambient_occlusion=EffectLevel::high; effects.ray_tracing=false;
    CHECK(!uc::rt_ui(effects,true,true).ao_replaced);
    effects.ray_tracing=true;
    CHECK(!uc::rt_ui(effects,true,false).ao_replaced);   // a GPU that cannot trace: the raster AO stays
    return 0;
}
