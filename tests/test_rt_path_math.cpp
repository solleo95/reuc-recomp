#include "check.h"
#include "rt_path_math.h"
#include <cmath>
int main() {
    const auto settings=uc::rt_path_settings();
    CHECK(settings.divisor>=1 && settings.paths>=1 && settings.bounces>=1 && settings.reach>0.f && settings.mix>=0.f && settings.mix<=1.f);
    for (const float channel : uc::kRtPathWarm) CHECK(channel>0.f && channel<=1.f);
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
    // The composition: albedo x light replaces the game's colour by `mix`, never exceeds 1, and the fog keeps the game's colour
    const std::array<float,3> game{0.2f,0.2f,0.2f}, albedo{0.5f,0.5f,0.5f}, light{1.f,2.f,4.f};
    const auto full=uc::rt_path_compose(game,albedo,light,1.f,0.f);
    CHECK(std::fabs(full[0]-0.5f)<1e-6f && std::fabs(full[1]-1.f)<1e-6f && std::fabs(full[2]-1.f)<1e-6f);   // 0.5, 1.0, and 2.0 clamped to 1
    CHECK(uc::rt_path_compose(game,albedo,light,0.f,0.f)==game && uc::rt_path_compose(game,albedo,light,1.f,1.f)==game);
    const auto half=uc::rt_path_compose(game,albedo,light,0.5f,0.f);
    CHECK(std::fabs(half[0]-0.35f)<1e-6f);
    const std::array<float,3> negative{-1.f,-1.f,-1.f};
    CHECK(uc::rt_path_compose(game,albedo,negative,1.f,0.f)[0]==0.f);   // negative light (a noisy signal) is clipped
    return 0;
}
