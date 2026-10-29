#include "check.h"
#include "wiimote_math.h"
#include <cmath>
#include <limits>
using namespace uc;
static std::array<WiiIrDot,4> two_dots(float x1,float y1,float x2,float y2) {
    std::array<WiiIrDot,4> dots{};
    dots[0]={uint16_t(std::lround(x1)),uint16_t(std::lround(y1)),true};
    dots[1]={uint16_t(std::lround(x2)),uint16_t(std::lround(y2)),true};
    return dots;
}
static bool near(float a,float b,float tolerance=1e-3f) { return std::abs(a-b)<=tolerance; }
static void pointer_centered_dots_gives_zero() {
    const auto pointer=wii_ir_pointer(two_dots(462,384,562,384));
    CHECK(pointer && near((*pointer)[0],0) && near((*pointer)[1],0));
}
static void pointer_mirrors_both_axes() {
    const auto right=wii_ir_pointer(two_dots(362,384,462,384));   // the bar appears left of the middle: the remote points right of it
    CHECK(right && (*right)[0]>.1f && near((*right)[1],0));
    const auto down=wii_ir_pointer(two_dots(462,284,562,284));    // the bar appears high: the remote points below it
    CHECK(down && (*down)[1]>.1f && near((*down)[0],0));
}
static void pointer_clamps_to_the_window() {
    const auto edge=wii_ir_pointer(two_dots(0,0,100,0));
    CHECK(edge && (*edge)[0]==1.f && (*edge)[1]==1.f);
}
static void pointer_needs_two_distinct_dots() {
    CHECK(!wii_ir_pointer({}));
    std::array<WiiIrDot,4> one{}; one[2]={500,300,true};
    CHECK(!wii_ir_pointer(one));
    CHECK(!wii_ir_pointer(two_dots(500,300,500,300)));
    CHECK(!wii_ir_pointer(two_dots(500,300,503,301)));   // closer than a sensor bar can look: one blob
}
static void pointer_uses_the_outer_pair() {
    std::array<WiiIrDot,4> dots{};
    dots[0]={400,380,true}; dots[1]={430,384,true}; dots[2]={590,384,true}; dots[3]={620,388,true};
    const auto pointer=wii_ir_pointer(dots);
    CHECK(pointer && std::abs((*pointer)[0])<.02f && std::abs((*pointer)[1])<.02f);   // the middle of (400,380) and (620,388)
}
static void pointer_is_compensated_for_roll() {
    const auto level=wii_ir_pointer(two_dots(362,334,462,334));
    CHECK(level);
    const float angle=30.f*3.14159265f/180;
    const auto turn=[&](float x,float y) { const float dx=x-512,dy=y-384; return std::array<float,2>{512+dx*std::cos(angle)-dy*std::sin(angle),384+dx*std::sin(angle)+dy*std::cos(angle)}; };
    const auto a=turn(362,334),b=turn(462,334);
    const auto rolled=wii_ir_pointer(two_dots(a[0],a[1],b[0],b[1]));
    CHECK(rolled && near((*rolled)[0],(*level)[0],.01f) && near((*rolled)[1],(*level)[1],.01f));
}
static void pointer_ignores_dot_order() {
    const auto first=wii_ir_pointer(two_dots(362,334,462,334));
    const auto swapped=wii_ir_pointer(two_dots(462,334,362,334));
    CHECK(first && swapped && near((*first)[0],(*swapped)[0]) && near((*first)[1],(*swapped)[1]));
}
static void stick_centre_range_and_dead_zone() {
    CHECK((wii_stick({128,128},.1f)==std::array<float,2>{0,0}));
    const auto right=wii_stick({255,128},.1f); CHECK(right[0]==1.f && right[1]==0.f);
    const auto up=wii_stick({128,255},.1f); CHECK(up[0]==0.f && up[1]==-1.f);   // the stick's raw y grows upward; the result is y down like SDL's sticks
    CHECK((wii_stick({128+8,128},.1f)==std::array<float,2>{0,0}));            // inside the dead zone
    const auto half=wii_stick({128+60,128},.1f); CHECK(half[0]>.4f && half[0]<.6f);   // rescaled past the dead zone
    const auto low=wii_stick({0,0},.1f); CHECK(low[0]==-1.f && low[1]==1.f);
}
static void swing_ignores_gravity_and_small_motion() {
    WiiSwing swing;
    uint64_t ns=1000000000;
    for(int i=0;i<100;++i,ns+=10000000) CHECK((swing.feed({0x82,0x82,0x9C},ns,1.5f)==std::array<float,2>{0,0}));
    CHECK((swing.feed({uint8_t(0x82+13),0x82,0x9C},ns,1.5f)==std::array<float,2>{0,0}));   // half a g of hand movement
}
static void swing_ignores_a_slow_tilt() {
    WiiSwing swing;
    uint64_t ns=1000000000;
    for(int i=0;i<150;++i,ns+=10000000) {   // the remote rolls over a second and a half: gravity moves from z to x
        const float roll=1.57f*float(i)/150;
        const uint8_t x=uint8_t(0x82+std::sin(roll)*26),z=uint8_t(0x82+std::cos(roll)*26);
        CHECK((swing.feed({x,0x82,z},ns,1.5f)==std::array<float,2>{0,0}));
    }
}
static void swing_big_jerk_emits_counts_in_its_direction() {
    WiiSwing swing;
    uint64_t ns=1000000000;
    for(int i=0;i<30;++i,ns+=10000000) swing.feed({0x82,0x82,0x9C},ns,1.5f);
    const auto right=swing.feed({uint8_t(0x82+100),0x82,0x9C},ns,1.5f);   // about 3.8 g to the right
    CHECK(right[0]>100 && std::abs(right[1])<1);
    ns+=10000000;
    const auto up=swing.feed({0x82,0x82,uint8_t(0x9C+60)},ns,1.5f);        // 2.3 g up: screen y goes negative
    CHECK(up[1]<-100 && std::abs(up[0])<30);
}
static void swing_threshold_follows_the_setting() {
    WiiSwing strict,loose;
    uint64_t ns=1000000000;
    for(int i=0;i<30;++i,ns+=10000000) { strict.feed({0x82,0x82,0x9C},ns,3.f); loose.feed({0x82,0x82,0x9C},ns,.8f); }
    const std::array<uint8_t,3> jerk{uint8_t(0x82+52),0x82,0x9C};   // 2 g
    CHECK((strict.feed(jerk,ns,3.f)==std::array<float,2>{0,0}));
    CHECK(loose.feed(jerk,ns,.8f)[0]>50);
}
static void swing_restarts_after_a_pause() {
    WiiSwing swing;
    swing.feed({0x82,0x82,0x9C},1000000000,1.5f);
    CHECK((swing.feed({uint8_t(0x82+100),0x82,0x9C},5000000000,1.5f)==std::array<float,2>{0,0}));   // four seconds later: a new stream, no baseline to compare with
}
int main() {
    pointer_centered_dots_gives_zero();
    pointer_mirrors_both_axes();
    pointer_clamps_to_the_window();
    pointer_needs_two_distinct_dots();
    pointer_uses_the_outer_pair();
    pointer_is_compensated_for_roll();
    pointer_ignores_dot_order();
    stick_centre_range_and_dead_zone();
    swing_ignores_gravity_and_small_motion();
    swing_ignores_a_slow_tilt();
    swing_big_jerk_emits_counts_in_its_direction();
    swing_threshold_follows_the_setting();
    swing_restarts_after_a_pause();
}
