#include "check.h"
#include "graphics_effects.h"
int main() {
    using uc::EffectLevel;
    uc::GraphicsEffects chosen;
    chosen.rt_shadows=EffectLevel::high; chosen.shadows=EffectLevel::medium; chosen.contact_shadows=true;
    chosen.ambient_occlusion=EffectLevel::low; chosen.rt_ambient_occlusion=EffectLevel::ultra;
    {   // master off: raster effects untouched, every RT level masked
        auto expected=chosen; expected.rt_shadows=expected.rt_ambient_occlusion=EffectLevel::off;
        CHECK(uc::effective_effects(chosen,true)==expected);
        CHECK(!uc::any_rt_active(chosen,true) && uc::rt_level(chosen,EffectLevel::high,true)==EffectLevel::off);
    }
    chosen.ray_tracing=true;
    {   // master on: RT replaces its raster counterparts
        const auto effective=uc::effective_effects(chosen,true);
        CHECK(effective.shadows==EffectLevel::off && !effective.contact_shadows && effective.ambient_occlusion==EffectLevel::off);
        CHECK(effective.rt_shadows==EffectLevel::high && effective.rt_ambient_occlusion==EffectLevel::ultra && effective.ray_tracing);
        CHECK(uc::any_rt_active(chosen,true) && uc::rt_level(chosen,EffectLevel::high,true)==EffectLevel::high);
    }
    {   // no device support: same as the master off
        auto expected=chosen; expected.rt_shadows=expected.rt_ambient_occlusion=EffectLevel::off;
        CHECK(uc::effective_effects(chosen,false)==expected);
        CHECK(!uc::any_rt_active(chosen,false));
    }
    {   // reflections alone leave the raster shadows and AO alone
        uc::GraphicsEffects only; only.ray_tracing=true; only.rt_reflections=EffectLevel::low;
        only.shadows=EffectLevel::medium; only.contact_shadows=true; only.ambient_occlusion=EffectLevel::low;
        const auto effective=uc::effective_effects(only,true);
        CHECK(effective.shadows==EffectLevel::medium && effective.contact_shadows && effective.ambient_occlusion==EffectLevel::low);
    }
    {   // scene targets
        uc::GraphicsEffects on; on.ray_tracing=true; on.anisotropy=1;
        CHECK(!uc::needs_scene_targets(on));
        on.rt_local_shadows=EffectLevel::low;
        CHECK(uc::needs_scene_targets(on) && uc::needs_scene_targets(uc::effective_effects(on,true)) && !uc::needs_scene_targets(uc::effective_effects(on,false)));
    }
    return 0;
}
