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
}
