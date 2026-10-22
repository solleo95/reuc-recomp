#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace uc {
// Bloom (graphics.bloom, graphics.bloom_strength): a six-level float glow around the bright parts of the scene. Mirrored in shaders/bloom.slang.
struct BloomSettings {
    bool enabled=true;
    uint32_t strength=100;   // percent of the base mix, 0..200
    bool operator==(const BloomSettings&) const=default;
};
inline constexpr uint32_t kBloomStrengthMax=200;
inline bool valid_bloom_settings(const BloomSettings& s) { return s.strength<=kBloomStrengthMax; }
// Starting values (docs/superpowers/specs/2026-10-22-bloom-design.md), tuned by eye in the game.
inline constexpr float kBloomKnee=.6f, kBloomSdrHeadroom=4.f, kBloomBaseMix=.15f, kBloomHdrGain=2.5f;
inline constexpr std::array<float,6> kBloomWeights{.20f,.20f,.18f,.16f,.14f,.12f};
// The game renders 8 bits, so a lamp and a white wall both read 1.0. Linear 0..1 above the knee rises to `headroom` (1.0 = paper white), with the
// same shape as hdr_expand: the square keeps the slope continuous at the knee.
inline float bloom_expand(float v, float headroom) {
    v=std::clamp(v,0.f,1.f);
    const float over=std::max(0.f,(v-kBloomKnee)/(1-kBloomKnee));
    return v+over*over*(headroom-1);
}
// What of a (scene-referred, expanded) luminance feeds the glow: with no threshold every lit wall adds its own value and the whole picture turns milky (first look in
// the train car), so only energy above kBloomThreshold counts, eased in by a quadratic knee. The shader applies the ratio to the colour.
inline constexpr float kBloomThreshold=.6f, kBloomThresholdKnee=.3f;
inline float bloom_soft_threshold(float luma) {
    const float m=std::clamp(luma-kBloomThreshold+kBloomThresholdKnee,0.f,2*kBloomThresholdKnee);
    return std::max(m*m/(4*kBloomThresholdKnee),luma-kBloomThreshold);
}
// How much of the pyramid's sum is added to the scene. Strength 0 and a switched-off bloom add nothing.
inline float bloom_mix(const BloomSettings& s, bool hdr) {
    if(!s.enabled || s.strength==0) return 0;
    return s.strength/100.f*kBloomBaseMix*(hdr ? kBloomHdrGain : 1.f);
}
// The first downsample averages with these weights, so one hot pixel cannot flicker a whole level.
inline float bloom_karis_weight(float luma) { return 1/(1+luma); }
struct BloomLevel { uint32_t width, height; };
// Level 0 is half the scene target, each next level half of the one before, never below one pixel (level 0..5).
inline BloomLevel bloom_level_extent(uint32_t width, uint32_t height, uint32_t level) {
    return {std::max(1u,width>>(level+1)),std::max(1u,height>>(level+1))};
}
}
