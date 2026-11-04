#include "check.h"
#include "raw_pointer_math.h"
#include <cmath>
using namespace uc;
static bool near(float a,float b,float tolerance=1e-4f) { return std::fabs(a-b)<=tolerance; }
static void a_window_that_fills_the_desktop_maps_the_corners() {
    const PixelRect desktop{0,0,1920,1080},window{0,0,1920,1080};
    auto at=raw_absolute_to_window(0,0,desktop,window); CHECK(near(at[0],0) && near(at[1],0));
    at=raw_absolute_to_window(65535,65535,desktop,window); CHECK(near(at[0],1) && near(at[1],1));
    at=raw_absolute_to_window(32768,32768,desktop,window); CHECK(near(at[0],.5f,2e-5f) && near(at[1],.5f,2e-5f));
}
static void a_window_inside_the_desktop_maps_its_own_edges_to_0_and_1() {
    const PixelRect desktop{0,0,1920,1080},window{320,180,1280,720};   // a 1280x720 window with its client area at (320,180)
    auto at=raw_absolute_to_window(int32_t(65535*320/1920.0),int32_t(65535*180/1080.0),desktop,window); CHECK(near(at[0],0,1e-3f) && near(at[1],0,1e-3f));
    at=raw_absolute_to_window(int32_t(65535*1600/1920.0),int32_t(65535*900/1080.0),desktop,window); CHECK(near(at[0],1,1e-3f) && near(at[1],1,1e-3f));
    at=raw_absolute_to_window(0,0,desktop,window); CHECK(at[0]==0 && at[1]==0);                     // outside the window: clamped, never negative
    at=raw_absolute_to_window(65535,65535,desktop,window); CHECK(at[0]==1 && at[1]==1);              // or beyond 1
}
static void the_virtual_desktop_and_a_second_monitor() {
    const PixelRect desktop{-1920,0,3840,1080},window{0,0,1920,1080};   // a left monitor at negative x; the window is on the right (primary) one
    auto at=raw_absolute_to_window(32768,0,desktop,window); CHECK(near(at[0],0,1e-3f));             // the middle of the virtual desktop is the left edge of the right monitor
    at=raw_absolute_to_window(65535,0,desktop,window); CHECK(near(at[0],1,1e-3f));
}
// The light gun rule: the position is linear in the raw value, so what is aimed at is hit from the middle of the screen to its edge (no growing offset).
static void the_mapping_is_linear_from_the_middle_to_the_edge() {
    const PixelRect desktop{0,0,2560,1080},window{0,0,2560,1080};
    float previous=raw_absolute_to_window(32768,0,desktop,window)[0],step=0; bool have=false;
    for(int raw=36044;raw<=65535;raw+=3277) {
        const float x=raw_absolute_to_window(raw,0,desktop,window)[0];
        if(have) CHECK(near(x-previous,step,1e-4f)); else { step=x-previous; have=true; }
        previous=x;
    }
}
static void a_window_without_size_is_the_middle() {
    const auto at=raw_absolute_to_window(100,100,PixelRect{0,0,1920,1080},PixelRect{0,0,0,0}); CHECK(at[0]==.5f && at[1]==.5f);
    const auto none=raw_absolute_to_window(100,100,PixelRect{0,0,0,0},PixelRect{0,0,100,100}); CHECK(none[0]==.5f && none[1]==.5f);
}
int main() {
    a_window_that_fills_the_desktop_maps_the_corners(); a_window_inside_the_desktop_maps_its_own_edges_to_0_and_1(); the_virtual_desktop_and_a_second_monitor();
    the_mapping_is_linear_from_the_middle_to_the_edge(); a_window_without_size_is_the_middle();
}
