#include "check.h"
#include "native_input.h"
#include "ppc_fixture.h"
#include <cmath>
#include <limits>

int main() {
    uc::NativeInput input;
    input.focus(true);
    CHECK(input.move_raw(100,50,1000,500));
    auto first=input.consume(); CHECK(std::abs(first.x-0.2f)<0.00001f && std::abs(first.y-0.2f)<0.00001f);
    CHECK(!input.move_raw(std::numeric_limits<float>::quiet_NaN(),0,1000,500));
    CHECK(!input.move_raw(1,1,0,500));
    input.button(uc::kButtonA,true); input.button(uc::kButtonA,false);
    auto click=input.consume(); CHECK(click.held==0 && click.pressed==uc::kButtonA && click.released==uc::kButtonA);
    CHECK(input.consume().pressed==0);
    input.button(uc::kButtonA,true); input.consume(); input.focus(false);
    auto lost=input.consume(); CHECK(!lost.valid && lost.held==0 && lost.pressed==0 && lost.released==uc::kButtonA);
    CHECK(!input.move_raw(50,0,1000,500)); input.focus(true);
    Fix fixture; auto& c=fixture.c; uc::input_hle_init(&input);
    c.r[3]=0; c.r[4]=0x80001000; hle_WPADProbe(c); CHECK(c.r[3]==0 && ppc::rd32(c,0x80001000)==0);
    input.button(uc::kButtonA,true);
    c.r[3]=0; c.r[4]=0xCC008000; c.r[5]=16; hle_KPADRead(c); CHECK(c.r[3]==0);
    c.r[3]=0; c.r[4]=0x80002000; c.r[5]=16; hle_KPADRead(c);
    CHECK(c.r[3]==1 && ppc::rd32(c,0x80002000)==0x800 && ppc::rd32(c,0x80002004)==0x800);
    CHECK(std::abs(ppc::rdf32(c,0x80002020)-0.2f)<0.00001f && ppc::rd8(c,0x8000205E)==1);
    c.r[3]=0; c.r[4]=0x80002000; c.r[5]=1; hle_KPADRead(c); CHECK(c.r[3]==1 && ppc::rd32(c,0x80002004)==0);
    c.r[3]=1; c.r[4]=0x80001000; hle_WPADProbe(c); CHECK(int32_t(c.r[3])==-1);
    uc::input_hle_init(nullptr);
    c.r[3]=0; c.r[4]=0x80002000; c.r[5]=16; hle_KPADRead(c); CHECK(c.r[3]==0);
}
