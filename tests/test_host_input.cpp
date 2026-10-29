#include "check.h"
#include "host_input.h"
static uint64_t bit(uc::Action a) { return uint64_t(1)<<unsigned(a); }
int main() {
    uc::NativeInput input; input.context(uc::InputContext::gameplay); input.focus(true);
    uc::HostInput host; host.extent(1000,500);
    SDL_Event e{}; e.type=SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.button=SDL_BUTTON_LEFT;
    CHECK(host.event(e,input,false)); CHECK(input.consume_actions().pressed==0);
    CHECK(!host.event(e,input,true)); CHECK(input.consume_actions().held==bit(uc::Action::interact));
    e.type=SDL_EVENT_MOUSE_BUTTON_UP; host.event(e,input,true); input.consume_actions();
    e.type=SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.button=SDL_BUTTON_RIGHT; host.event(e,input,true);
    CHECK(input.consume_actions().pressed==bit(uc::Action::fire));
    e.type=SDL_EVENT_KEY_DOWN; e.key.scancode=SDL_SCANCODE_W; e.key.key=SDLK_E;
    host.event(e,input,true); CHECK(input.consume_actions().pressed==bit(uc::Action::weapon_previous));
    e.key.repeat=true; host.event(e,input,true); CHECK(input.consume_actions().pressed==0);
    e.key.repeat=false; e.key.scancode=SDL_SCANCODE_E; e.key.key=SDLK_W;
    host.event(e,input,true); CHECK(input.consume_actions().pressed==0);
    e={}; e.type=SDL_EVENT_MOUSE_WHEEL; e.wheel.y=-2; host.event(e,input,true);
    auto frame=input.consume_actions(); CHECK(frame.count==4 && frame.events[0].action==uc::Action::weapon_next);
    e.wheel.direction=SDL_MOUSEWHEEL_FLIPPED; e.wheel.y=-1; host.event(e,input,true);
    frame=input.consume_actions(); CHECK(frame.count==1 && frame.events[0].pulse && frame.events[0].action==uc::Action::weapon_previous);
    e={}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.xrel=100; e.motion.yrel=50;
    host.event(e,input,true); const auto aim=input.consume(); CHECK(aim.x==.2f && aim.y==.2f);
    e.type=SDL_EVENT_WINDOW_FOCUS_LOST; host.event(e,input,true);
    CHECK(input.consume_actions().held==0 && !input.consume().valid);
    e.type=SDL_EVENT_WINDOW_FOCUS_GAINED; host.event(e,input,false);
    e.type=SDL_EVENT_MOUSE_MOTION; e.motion.xrel=100; host.event(e,input,false); CHECK(input.consume().x==.2f);
    input.focus(true); const auto base=input.consume();
    host.sensitivity(50); host.event(e,input,true); const auto slow=input.consume();
    CHECK(std::abs(slow.x-base.x-.1f)<1e-5f && std::abs(slow.y-base.y-.1f)<1e-5f);   // half of the .2 / .2 that 100 / 50 px move at 100%
    {   // absolute pointer (lightguns): the window position of the cursor is the pointer, relative deltas mean nothing
        uc::NativeInput gun; gun.context(uc::InputContext::gameplay); gun.focus(true);
        uc::HostInput absolute; absolute.extent(1000,500); absolute.pointer(uc::PointerMode::absolute);
        e={}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.x=500; e.motion.y=250; e.motion.xrel=7; e.motion.yrel=7;
        absolute.event(e,gun,true); CHECK((gun.latest_aim()==std::array<float,2>{0,0}));
        e.motion.x=0; e.motion.y=0; absolute.event(e,gun,true); CHECK((gun.latest_aim()==std::array<float,2>{-1,-1}));
        e.motion.xrel=100; e.motion.yrel=100; absolute.event(e,gun,true); CHECK((gun.latest_aim()==std::array<float,2>{-1,-1}));   // same place: the deltas are ignored
        absolute.density(2); e.motion.x=125; e.motion.y=62.5f; absolute.event(e,gun,true);   // 150% desktops report points: the pixels are density times as many
        CHECK(std::abs(gun.latest_aim()[0]+.5f)<1e-5f && std::abs(gun.latest_aim()[1]+.5f)<1e-5f);
        e.motion.x=500; e.motion.y=250; absolute.event(e,gun,false); CHECK(std::abs(gun.latest_aim()[0]+.5f)<1e-5f);   // not captured: the pointer stays
        e={}; e.type=SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.button=SDL_BUTTON_LEFT;
        CHECK(absolute.event(e,gun,false)); CHECK(gun.consume_actions().pressed==0);   // the first click only takes the window back
        CHECK(!absolute.event(e,gun,true)); CHECK(gun.consume_actions().held==bit(uc::Action::interact));
        {   // a 200% display: the swing detectors still see the points a mouse would, not the doubled pixels
            uc::NativeInput hi; hi.context(uc::InputContext::gameplay); hi.focus(true); hi.host_button({uc::PhysicalKind::mouse,1},true);
            uc::HostInput dpi; dpi.extent(1000,1000); dpi.pointer(uc::PointerMode::absolute); dpi.density(2);
            e={}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.x=250; e.motion.y=250; e.motion.timestamp=1000000000; dpi.event(e,hi,true);
            e.motion.y=230; e.motion.timestamp=1008000000; dpi.event(e,hi,true);   // 20 points = 40 pixels in 8 ms
            CHECK((hi.consume_actions().pressed&bit(uc::Action::knife))==0);
        }
        uc::HostInput relative; relative.extent(1000,500);   // the default is still the relative mouse
        e={}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.x=900; e.motion.y=400; e.motion.xrel=100; e.motion.yrel=50;
        const auto before=gun.latest_aim(); relative.event(e,gun,true);
        CHECK(std::abs(gun.latest_aim()[0]-before[0]-.2f)<1e-5f && std::abs(gun.latest_aim()[1]-before[1]-.2f)<1e-5f);
    }
}
