#pragma once
#include "graphics_effects.h"
namespace uc {
// What the ray tracing group of the launcher and the in-game menu may do: one checkbox ("ray tracing", which is path tracing: GraphicsEffects::path_tracing) and the quality
// select under it (GraphicsEffects::rt_quality). `known`: the feature report has arrived; `supported`: the GPU can trace rays (FeatureReport.ray_tracing). Nothing is blocked while the report is pending.
struct RtUi {
    bool master_enabled;     // the checkbox: usable unless the GPU cannot trace; a saved "on" can always be switched off
    bool quality_enabled;    // the quality select: the checkbox is on and the GPU can trace (the renderer masks it off otherwise)
    bool raster_replaced;    // the shadow map, the contact shadows and GTAO are greyed: path tracing lights the scene instead (the saved values stay)
};
inline RtUi rt_ui(const GraphicsEffects& effects, bool known, bool supported) {
    const bool blocked=known && !supported;
    const bool active=effects.path_tracing && !blocked;
    return {!blocked || effects.path_tracing, active, active};
}
}
