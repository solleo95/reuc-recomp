#include "check.h"
#include "rt_material.h"
// TEV colour env words (tev.slang): d | c<<4 | b<<8 | a<<12, bit 19 clamp, bits 22-23 output register. Selectors: 0 = previous, 8 = texture,
// 10 = rasterised colour, 11 = rasterised alpha, 14 = konst, 15 = zero.
constexpr uint32_t env(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return d|(c<<4)|(b<<8)|(a<<12)|(1u<<19); }
constexpr uint32_t kTextureOrder=64;   // texture map 0, coordinate 0, colour channel 0
int main() {
    const auto lit=gx::channel_lighting(2|(1<<2));   // lighting on, light 0
    const auto unlit=gx::channel_lighting(1);
    gx::TevMaterial modulate; modulate.stages=1; modulate.swaps={0xE4,0xE4,0xE4,0xE4}; modulate.stage[0].color=env(15,8,10,15); modulate.stage[0].order=kTextureOrder;   // texture x rasterised
    CHECK(uc::light_model_valid(modulate,lit));
    gx::TevMaterial konst=modulate; konst.stage[0].color=env(15,10,14,15);   // rasterised x konst
    CHECK(uc::light_model_valid(konst,lit));
    gx::TevMaterial pass=modulate; pass.stages=2; pass.stage[1].color=env(15,15,15,0); pass.stage[1].order=kTextureOrder;   // a second stage passing the previous result through
    CHECK(uc::light_model_valid(pass,lit));
    gx::TevMaterial changed=pass; changed.stage[1].color=env(15,15,15,8);   // ... while a second stage replacing it is not
    CHECK(!uc::light_model_valid(changed,lit));
    CHECK(!uc::light_model_valid(modulate,unlit));   // an unlit channel 0: baked colour
    gx::TevMaterial additive=modulate; additive.stage[0].color=env(8,15,15,10);   // texture + rasterised: a light-independent term next to a lit one is accepted (it dims with the lit part)
    CHECK(uc::light_model_valid(additive,lit));
    gx::TevMaterial only_factors=modulate; only_factors.stage[0].color=env(8,15,15,12);   // texture + one: nothing follows the light
    CHECK(!uc::light_model_valid(only_factors,lit));
    gx::TevMaterial alpha_only=modulate; alpha_only.stage[0].color=env(15,8,11,15);   // the rasterised colour used only as alpha
    CHECK(!uc::light_model_valid(alpha_only,lit));
    gx::TevMaterial other_channel=modulate; other_channel.stage[0].order=kTextureOrder|(1u<<7);   // rasterised colour of channel 1
    CHECK(!uc::light_model_valid(other_channel,lit));
    gx::TevMaterial scaled=modulate; scaled.stage[0].color|=1u<<20;   // scale 2: a constant factor on the lit colour, the shadow factor still multiplies it
    CHECK(uc::light_model_valid(scaled,lit));
    gx::TevMaterial biased=modulate; biased.stage[0].color|=1u<<16;   // bias +0.5: a term that does not follow the light
    CHECK(!uc::light_model_valid(biased,lit));
    gx::TevMaterial subtracted=modulate; subtracted.stage[0].color|=1u<<18;
    CHECK(!uc::light_model_valid(subtracted,lit));
    gx::TevMaterial squared=modulate; squared.stage[0].color=env(15,10,10,15);   // rasterised x rasterised is not proportional to the light
    CHECK(!uc::light_model_valid(squared,lit));
    gx::TevMaterial constant_added=pass; constant_added.stage[1].color=env(15,8,12,0);   // previous + texture x one: a light-independent term next to the lit result
    CHECK(uc::light_model_valid(constant_added,lit));   // accepted: the shadow factor dims it with the lit part (the game's seats, floors and walls carry such a term)
    // The shapes of the game's own draws (fight run, 3.1 M records: none of them was model-valid before the classification followed the TEV algebra)
    const auto game=[&](uint32_t stages,uint32_t color0,uint32_t color1,uint32_t order) {
        gx::TevMaterial material; material.stages=stages; material.swaps={0xE4,0xE4,0xE4,0xE4};
        for(uint32_t i=0;i<stages;++i) { material.stage[i].color=i==0 ? color0 : i==1 ? color1 : env(15,15,15,0); material.stage[i].order=order; }
        return material;
    };
    CHECK(uc::light_model_valid(game(2,0x8F8AF,0x28F8A0,0x40),lit));    // texture x lit, then + texture x lit again, x4
    CHECK(uc::light_model_valid(game(1,0x28F8AF,0,0x40),lit));          // texture x lit, x4
    CHECK(uc::light_model_valid(game(2,0x8F8AF,0x28F80F,0x49),lit));    // texture x lit, then x texture
    CHECK(uc::light_model_valid(game(4,0x8F8AF,0x28F80F,0x49),lit));    // ... followed by two pass-through stages
    CHECK(uc::light_model_valid(game(2,0x18F8AF,0x18FEA0,0x40),lit));   // texture x lit, then + konst x lit
    CHECK(uc::light_model_valid(game(2,0x18F8AF,0x18FAE0,0x40),lit));   // ... in the other operand order
    // The dining car's seats, floor and walls (first scene: 13.7 M of 44 M indices, none of them valid before): texture x lit, x texture x4, then + texture2 x konst
    {
        gx::TevMaterial seat; seat.stages=3; seat.swaps={0xE4,0xE4,0xE4,0xE4};
        seat.stage[0].color=0x8F8AF; seat.stage[0].order=0x49; seat.stage[1].color=0x28F80F; seat.stage[1].order=0x3C0; seat.stage[2].color=0x8F8E0; seat.stage[2].order=0x3D2;
        CHECK(uc::light_model_valid(seat,lit));
        gx::TevMaterial seat_specular=seat; seat_specular.stages=4; seat_specular.stage[3].color=0x8AFF0; seat_specular.stage[3].order=0xBF;   // ... and + ras1
        CHECK(uc::light_model_valid(seat_specular,lit,gx::channel_lighting(2|(1<<2)|(1<<9))));
    }
    // Channel 1 is the specular channel (attenuation function 1) of the game's draws: reading its rasteriser colour is a highlight that follows the same lights
    const auto specular=gx::channel_lighting(2|(1<<2)|(1<<9)), diffuse1=gx::channel_lighting(2|(1<<2)|(3<<9)), unlit1=gx::channel_lighting(0);
    const auto two_stage=[&](uint32_t color0,uint32_t order0,uint32_t color1,uint32_t order1) {
        gx::TevMaterial material; material.stages=2; material.swaps={0xE4,0xE4,0xE4,0xE4};
        material.stage[0].color=color0; material.stage[0].order=order0; material.stage[1].color=color1; material.stage[1].order=order1;
        return material;
    };
    CHECK(uc::light_model_valid(two_stage(0x8F8AF,0x40,0x28F8A0,0xC9),lit,specular));    // texture x lit, + texture x ras1 (the 822 k records of the fight demo)
    CHECK(uc::light_model_valid(two_stage(0x28F8AF,0x40,0x8AFF0,0xBF),lit,specular));    // texture x lit x4, + ras1
    CHECK(!uc::light_model_valid(two_stage(0x8F8AF,0x40,0x28F8A0,0xC9),lit,diffuse1));   // a diffuse second channel is another light set the one factor cannot scale
    CHECK(!uc::light_model_valid(two_stage(0x8F8AF,0x40,0x28F8A0,0xC9),lit,unlit1));     // ... and an unlit one is a baked colour
    CHECK(!uc::light_model_valid(two_stage(0x8F8AF,0x40,0x28F8A0,0xC9),lit));            // the default second channel is unlit: the old two-argument behaviour
    CHECK(!uc::light_model_valid(two_stage(0x8F8AF,0x40,env(15,10,10,0),0xC9),lit,specular));   // prev + ras1 x ras1 is not proportional
    CHECK(!uc::light_model_valid(two_stage(0x8F8AF,0x40,0x28F8A0,0x149),lit,specular));  // channel 2 does not exist as a lit channel
    const auto vertex_ambient=gx::channel_lighting(2|(1<<2)|64);       // the ambient taken from the vertex colour, not the register the record stores
    CHECK(!uc::light_model_valid(modulate,vertex_ambient));
    gx::TevMaterial swapped=modulate; swapped.stage[0].alpha=1; swapped.swaps[1]=0x1B;   // rasteriser swap table 1 reverses the channels
    CHECK(!uc::light_model_valid(swapped,lit));
    gx::TevMaterial none; CHECK(uc::light_model_valid(none,lit));   // no TEV stage: the renderer draws rasterised x texture 0
    CHECK(!uc::light_model_valid(none,unlit));
    CHECK(sizeof(uc::RtMaterialRecord)==48);
    // The record carries the channel's attenuation function (bits 8-9) and diffuse mode (10-11) for the RT shadow pass (shaders/rt_shadow.slang)
    const auto channel=gx::channel_lighting(2|(1<<2)|(2<<7)|(3<<9));   // lit, light 0, clamped diffuse, distance attenuation
    const uint32_t flags=uc::rt_record_flags(channel,true);
    CHECK((flags&uc::kRtModelValid) && ((flags>>8)&3)==3 && ((flags>>10)&3)==2);
    CHECK(!(uc::rt_record_flags(channel,false)&uc::kRtModelValid) && ((uc::rt_record_flags(channel,false)>>8)&3)==3);
    CHECK(((uc::rt_record_flags(gx::channel_lighting(2|(1<<2)),false)>>8)&15)==0);   // no attenuation, no diffuse
    return 0;
}
