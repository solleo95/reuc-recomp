#pragma once
#include "graphics_effects.h"
namespace uc {
// What the ray tracing group of the launcher and the in-game menu may do (docs/superpowers/plans/2026-10-19-ray-tracing-p2.md, Task 10). `known`: the feature
// report has arrived; `supported`: the GPU can trace rays (FeatureReport.ray_tracing). Nothing is blocked while the report is pending.
struct RtUi {
    bool master_enabled;     // the master checkbox: usable unless the GPU cannot trace; a saved "on" can always be switched off
    bool levels_enabled;     // the level combos: the master is on and the GPU can trace (the renderer masks RT off otherwise)
    bool contact_replaced;   // contact shadows are greyed: active ray-traced shadows replace them (the saved value stays)
    bool shadows_replaced;   // the shadow map likewise
    bool ao_replaced;        // the raster ambient occlusion (GTAO) likewise, by the ray-traced one
};
inline RtUi rt_ui(const GraphicsEffects& effects, bool known, bool supported) {
    const bool blocked=known && !supported;
    const bool active=effects.ray_tracing && !blocked;
    const bool shadows=active && effects.rt_shadows!=EffectLevel::off;
    return {!blocked || effects.ray_tracing, active, shadows, shadows, active && effects.rt_ambient_occlusion!=EffectLevel::off};
}
}
