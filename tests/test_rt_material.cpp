#include "check.h"
#include "rt_material.h"
// TEV colour env words (tev.slang): d | c<<4 | b<<8 | a<<12, bit 19 clamp, bits 22-23 output register. Selectors: 0 = previous, 8 = texture,
// 10 = rasterised colour, 11 = rasterised alpha, 14 = konst, 15 = zero.
constexpr uint32_t env(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return d|(c<<4)|(b<<8)|(a<<12)|(1u<<19); }
constexpr uint32_t kTextureOrder=64;   // texture map 0, coordinate 0, colour channel 0
int main() {
    const auto lit=gx::channel_lighting(2|(1<<2));   // lighting on, light 0
    const auto unlit=gx::channel_lighting(1);
    gx::TevMaterial modulate; modulate.stages=1; modulate.stage[0].color=env(15,8,10,15); modulate.stage[0].order=kTextureOrder;   // texture x rasterised
    CHECK(uc::light_model_valid(modulate,lit));
    gx::TevMaterial konst=modulate; konst.stage[0].color=env(15,10,14,15);   // rasterised x konst
    CHECK(uc::light_model_valid(konst,lit));
    gx::TevMaterial pass=modulate; pass.stages=2; pass.stage[1].color=env(15,15,15,0); pass.stage[1].order=kTextureOrder;   // a second stage passing the previous result through
    CHECK(uc::light_model_valid(pass,lit));
    gx::TevMaterial changed=pass; changed.stage[1].color=env(15,15,15,8);   // ... while a second stage replacing it is not
    CHECK(!uc::light_model_valid(changed,lit));
    CHECK(!uc::light_model_valid(modulate,unlit));   // an unlit channel 0: baked colour
    gx::TevMaterial additive=modulate; additive.stage[0].color=env(8,15,15,10);   // texture + rasterised
    CHECK(!uc::light_model_valid(additive,lit));
    gx::TevMaterial alpha_only=modulate; alpha_only.stage[0].color=env(15,8,11,15);   // the rasterised colour used only as alpha
    CHECK(!uc::light_model_valid(alpha_only,lit));
    gx::TevMaterial other_channel=modulate; other_channel.stage[0].order=kTextureOrder|(1u<<7);   // rasterised colour of channel 1
    CHECK(!uc::light_model_valid(other_channel,lit));
    gx::TevMaterial scaled=modulate; scaled.stage[0].color|=1u<<20;   // scale 2
    CHECK(!uc::light_model_valid(scaled,lit));
    gx::TevMaterial none; CHECK(!uc::light_model_valid(none,lit));   // no TEV stage
    CHECK(sizeof(uc::RtMaterialRecord)==48);
    return 0;
}
