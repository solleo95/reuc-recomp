#include "check.h"
#include "rt_path_math.h"
#include <cmath>
int main() {
    const auto settings=uc::rt_path_settings();
    CHECK(settings.divisor>=1 && settings.paths>=1 && settings.bounces>=1 && settings.reach>0.f && settings.mix>=0.f && settings.mix<=1.f);
    for (const float channel : uc::kRtPathWarm) CHECK(channel>0.f && channel<=1.f);
    // Contact occlusion: short rays around the surface; the fraction that is blocked darkens by `ao_strength`, and the push constants pack the small counts
    CHECK(settings.ao_rays>=1 && settings.ao_radius>0.f && settings.ao_radius<settings.reach && settings.ao_strength>0.f && settings.ao_strength<=1.f);
    CHECK(uc::rt_path_ao(0.f,0.7f)==1.f && std::fabs(uc::rt_path_ao(1.f,0.7f)-0.3f)<1e-6f && std::fabs(uc::rt_path_ao(0.5f,0.7f)-0.65f)<1e-6f && uc::rt_path_ao(2.f,1.f)==0.f && uc::rt_path_ao(-1.f,1.f)==1.f);
    const auto packed=uc::rt_path_pack(settings);
    CHECK((packed.divisor_frame&255u)==settings.divisor && (packed.counts&255u)==settings.paths && ((packed.counts>>8)&255u)==settings.bounces && ((packed.counts>>16)&255u)==settings.ao_rays);
    CHECK(uc::rt_path_pack(settings,63).divisor_frame>>8==63u);
    // The mean albedo: the sums of 8-bit channels over the pixel count, a grey fallback for a record that is not on screen
    const auto mean=uc::rt_path_mean_albedo({510,255,0},3);
    CHECK(std::fabs(mean[0]-2.f/3.f)<1e-6f && std::fabs(mean[1]-1.f/3.f)<1e-6f && mean[2]==0.f);
    const std::array<float,3> grey{0.4f,0.4f,0.4f};
    CHECK(uc::rt_path_mean_albedo({9,9,9},0)==grey);
    // An emitter: an unlit draw (kRtEmissive) with a bright mean albedo; a lit draw or a dark unlit one is not
    const std::array<float,3> bright{0.9f,0.8f,0.6f}, dark{0.1f,0.1f,0.1f};
    CHECK(uc::rt_path_emits(bright,settings.threshold) && !uc::rt_path_emits(dark,settings.threshold));   // lit or unlit: what counts is the colour the game shows
    // The clamp of one path's radiance (a firefly guard): per channel, never negative
    const std::array<float,3> spike{40.f,1.f,-2.f};
    const auto clamped=uc::rt_path_clamp(spike,settings.max_radiance);
    CHECK(clamped[0]==settings.max_radiance && clamped[1]==1.f && clamped[2]==0.f && settings.max_radiance>1.f);
    // The composition scales the game's own colour (its textures survive) by the gathered light relative to a reference white, within [kRtPathMinScale, kRtPathMaxScale];
    // `mix` blends the scale toward 1 and the fog keeps the game's colour
    CHECK(uc::kRtPathMinScale>0.f && uc::kRtPathMinScale<1.f && uc::kRtPathMaxScale>1.f && uc::kRtPathReference>0.f);
    const std::array<float,3> game{0.2f,0.4f,0.8f};
    const float ref=uc::kRtPathReference;
    const std::array<float,3> reference_light{ref,ref,ref};
    CHECK(uc::rt_path_compose(game,reference_light,1.f,0.f)==game);   // the reference light changes nothing
    const std::array<float,3> mixed_light{100.f,ref*0.5f,-1.f};
    const auto full=uc::rt_path_compose(game,mixed_light,1.f,0.f);
    CHECK(std::fabs(full[0]-game[0]*uc::kRtPathMaxScale)<1e-6f && std::fabs(full[1]-game[1]*0.5f)<1e-6f && std::fabs(full[2]-game[2]*uc::kRtPathMinScale)<1e-6f);   // clamped above, proportional, clamped below
    CHECK(uc::rt_path_compose(game,mixed_light,0.f,0.f)==game && uc::rt_path_compose(game,mixed_light,1.f,1.f)==game);
    const auto half=uc::rt_path_compose(game,mixed_light,0.5f,0.f);
    CHECK(std::fabs(half[0]-game[0]*(1.f+0.5f*(uc::kRtPathMaxScale-1.f)))<1e-6f);
    // the texture detail survives: two game colours keep their ratio under the same light
    const auto a=uc::rt_path_compose({0.2f,0.2f,0.2f},mixed_light,1.f,0.f), b=uc::rt_path_compose({0.4f,0.4f,0.4f},mixed_light,1.f,0.f);
    CHECK(std::fabs(b[1]/a[1]-2.f)<1e-5f);
    return 0;
}
