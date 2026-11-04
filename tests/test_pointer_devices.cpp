#include "check.h"
#include "pointer_feed.h"
#include <cmath>
#include <limits>
using namespace uc;
static PointerEvent button(uint8_t which,bool down) { PointerEvent e; e.kind=PointerEvent::Kind::button; e.device=7; e.button=which; e.down=down; return e; }
static PointerEvent relative(float x,float y) { PointerEvent e; e.kind=PointerEvent::Kind::relative; e.device=7; e.x=x; e.y=y; return e; }
static PointerEvent absolute(float x,float y) { PointerEvent e; e.kind=PointerEvent::Kind::absolute; e.device=7; e.x=x; e.y=y; return e; }
static PointerEvent wheel(float steps) { PointerEvent e; e.kind=PointerEvent::Kind::wheel; e.device=7; e.x=steps; return e; }
static bool near(float a,float b,float tolerance=1e-5f) { return std::fabs(a-b)<=tolerance; }
static void a_wheel_turns_the_weapons_like_the_mouse_wheel() {   // Windows takes the legacy wheel message away from a mouse that is read by Raw Input: the wheel arrives as an event
    const PointerWindow window{5,1280,720,1};
    const auto sdl=pointer_event_to_sdl(wheel(-2),window);
    CHECK(sdl.type==SDL_EVENT_MOUSE_WHEEL && sdl.wheel.y==-2);
    CHECK(pointer_event_to_sdl(wheel(std::numeric_limits<float>::quiet_NaN()),window).type==SDL_EVENT_USER);
    const auto big=pointer_event_to_sdl(wheel(32767.f/120),window);       // a wheel that reports 273 notches at once (bad hardware) is a long turn, not an error that stops the game
    CHECK(big.type==SDL_EVENT_MOUSE_WHEEL && big.wheel.y==100 && pointer_event_to_sdl(wheel(-5000),window).wheel.y==-100);
    NativeInput input; input.extent(1280,720); input.context(InputContext::gameplay); input.focus(true);
    HostInput host;
    CHECK(!feed_pointer_event(host,input,wheel(32767.f/120),window,true)); input.consume_actions();   // does not throw
    feed_pointer_event(host,input,wheel(-2),window,true);
    const auto frame=input.consume_actions();
    CHECK(frame.count==4 && frame.events[0].action==Action::weapon_next);                      // as test_host_input's wheel of -2
}
static void events_become_sdl_mouse_events() {
    const PointerWindow window{5,1280,720,1};
    auto sdl=pointer_event_to_sdl(button(SDL_BUTTON_RIGHT,true),window);
    CHECK(sdl.type==SDL_EVENT_MOUSE_BUTTON_DOWN && sdl.button.button==SDL_BUTTON_RIGHT && sdl.button.down && sdl.button.windowID==5);
    sdl=pointer_event_to_sdl(button(SDL_BUTTON_LEFT,false),window);
    CHECK(sdl.type==SDL_EVENT_MOUSE_BUTTON_UP && !sdl.button.down);
    sdl=pointer_event_to_sdl(relative(12,-3),window);
    CHECK(sdl.type==SDL_EVENT_MOUSE_MOTION && sdl.motion.xrel==12 && sdl.motion.yrel==-3);
    sdl=pointer_event_to_sdl(absolute(.5f,.5f),window);
    CHECK(sdl.type==SDL_EVENT_MOUSE_MOTION && near(sdl.motion.x,640) && near(sdl.motion.y,360));
    sdl=pointer_event_to_sdl(absolute(.5f,.5f),PointerWindow{5,2560,1440,2});   // a window of 2560x1440 pixels at 2 pixels per point is 1280x720 points
    CHECK(near(sdl.motion.x,640) && near(sdl.motion.y,360));
}
static void impossible_positions_are_dropped() {
    const PointerWindow window{5,1280,720,1};
    const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
    for(auto e:{absolute(nan,.5f),absolute(.5f,inf),absolute(1.5f,.5f),absolute(.5f,-.1f),relative(nan,0),relative(0,-inf)})
        CHECK(pointer_event_to_sdl(e,window).type==SDL_EVENT_USER);
    CHECK(pointer_event_to_sdl(absolute(0,1),window).type==SDL_EVENT_MOUSE_MOTION);          // the window's corner is a position
    CHECK(pointer_event_to_sdl(button(0,true),window).type==SDL_EVENT_USER);                 // button 0 is nothing
    CHECK(pointer_event_to_sdl(absolute(.5f,.5f),PointerWindow{5,0,0,1}).type==SDL_EVENT_USER);   // no window yet
}
// The light gun rule: the second player's absolute position goes through the same HostInput and NativeInput code as the first player's and lands exactly on
// (2*position-1)*edge: no distance is added up, so there is no offset that grows from the middle of the screen towards its edges.
static void an_absolute_gun_of_the_second_player_matches_the_first_players_path_everywhere() {
    for(const PointerWindow window:{PointerWindow{1,1280,720,1},PointerWindow{1,2560,1080,1.5f},PointerWindow{1,1000,500,2}}) {
        NativeInput first,second; for(auto* input:{&first,&second}) { input->extent(window.width,window.height); input->context(InputContext::gameplay); input->focus(true); }
        HostInput host_first,host_second;
        host_first.extent(window.width,window.height); host_first.density(window.density); host_first.pointer(PointerMode::absolute);
        float previous_x=0,previous_step=0; bool have_step=false;
        for(int step=0;step<=10;++step) {                               // from the middle (.5) to the edge (1) and back to the other edge (0)
            const float nx=step<=5 ? .5f+.1f*step : 1.f-.1f*(step-5)*2, ny=std::clamp(1.f-nx,0.f,1.f);
            SDL_Event e{}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.x=nx*window.width/window.density; e.motion.y=ny*window.height/window.density;
            host_first.event(e,first,true);
            feed_pointer_event(host_second,second,absolute(nx,ny),window,true);
            const auto a=first.latest_aim(),b=second.latest_aim();
            CHECK(near(a[0],b[0]) && near(a[1],b[1]));                  // the same code path gives the same pointer
            CHECK(near(b[0],std::clamp(2*nx-1,-1.f,1.f)*kAimEdgeX,2e-5f) && near(b[1],std::clamp(2*ny-1,-1.f,1.f)*kAimEdgeY,2e-5f));   // and it is the position, not a sum
            if(step>0 && step<=5) {                                       // a constant slope from the middle to the edge: nothing accumulates
                const float delta=b[0]-previous_x;
                if(have_step) CHECK(near(delta,previous_step,3e-5f));
                previous_step=delta; have_step=true;
            }
            previous_x=b[0];
        }
    }
}
static void a_relative_mouse_of_the_second_player_moves_only_the_second_pointer() {
    const PointerWindow window{1,1000,500,1};
    NativeInput first,second; for(auto* input:{&first,&second}) { input->extent(1000,500); input->context(InputContext::gameplay); input->focus(true); }
    HostInput host; host.extent(1000,500);
    feed_pointer_event(host,second,relative(100,50),window,true);
    const auto moved=second.consume();
    CHECK(near(moved.x,.2f) && near(moved.y,.2f) && first.consume().x==0);   // 100 and 50 counts of a 1000x500 window: .2 each (the first player's mouse does the same)
    feed_pointer_event(host,second,button(SDL_BUTTON_RIGHT,true),window,true);
    CHECK(second.consume_actions().pressed==uint64_t(1)<<unsigned(Action::fire) && first.consume_actions().pressed==0);
    feed_pointer_event(host,second,absolute(1,1),window,true);                 // a position after moves: the mode follows the event
    CHECK(near(second.latest_aim()[0],kAimEdgeX) && near(second.latest_aim()[1],kAimEdgeY));
    feed_pointer_event(host,second,relative(-100,-50),window,true);             // and back to moves
    CHECK(near(second.latest_aim()[0],kAimEdgeX-.2f));
}
int main() {
    a_wheel_turns_the_weapons_like_the_mouse_wheel(); events_become_sdl_mouse_events(); impossible_positions_are_dropped(); an_absolute_gun_of_the_second_player_matches_the_first_players_path_everywhere();
    a_relative_mouse_of_the_second_player_moves_only_the_second_pointer();
}
