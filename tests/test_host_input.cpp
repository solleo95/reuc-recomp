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
        e.motion.x=0; e.motion.y=0; absolute.event(e,gun,true); CHECK((gun.latest_aim()==std::array<float,2>{-.8f,-.6f}));   // the window's corner is the game's (-.8,-.6)
        e.motion.xrel=100; e.motion.yrel=100; absolute.event(e,gun,true); CHECK((gun.latest_aim()==std::array<float,2>{-.8f,-.6f}));   // same place: the deltas are ignored
        absolute.density(2); e.motion.x=125; e.motion.y=62.5f; absolute.event(e,gun,true);   // 150% desktops report points: the pixels are density times as many
        CHECK(std::abs(gun.latest_aim()[0]+.4f)<1e-5f && std::abs(gun.latest_aim()[1]+.3f)<1e-5f);
        e.motion.x=500; e.motion.y=250; absolute.event(e,gun,false); CHECK(std::abs(gun.latest_aim()[0]+.4f)<1e-5f);   // not captured: the pointer stays
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
    {   // touchscreen: a tap puts the aim on the finger and fires, a held finger drags the aim along with the fire held, and lifting it stops the fire
        uc::NativeInput deck; deck.context(uc::InputContext::gameplay); deck.focus(true);
        uc::HostInput touch; touch.extent(1000,500);
        e={}; e.type=SDL_EVENT_FINGER_DOWN; e.tfinger.touchID=1; e.tfinger.fingerID=7; e.tfinger.x=.5f; e.tfinger.y=.5f;
        touch.event(e,deck,true); CHECK((deck.latest_aim()==std::array<float,2>{0,0}));
        CHECK(deck.consume_actions().held==bit(uc::Action::fire));
        e.type=SDL_EVENT_FINGER_MOTION; e.tfinger.x=0; e.tfinger.y=0; touch.event(e,deck,true);
        CHECK((deck.latest_aim()==std::array<float,2>{-.8f,-.6f})); CHECK(deck.consume_actions().held==bit(uc::Action::fire));   // dragged to the corner, still firing
        e.type=SDL_EVENT_FINGER_DOWN; e.tfinger.fingerID=8; e.tfinger.x=1; e.tfinger.y=1; touch.event(e,deck,true);
        CHECK((deck.latest_aim()==std::array<float,2>{-.8f,-.6f}));                                                           // a second finger is ignored
        e.type=SDL_EVENT_FINGER_UP; e.tfinger.fingerID=7; e.tfinger.x=0; e.tfinger.y=0; touch.event(e,deck,true);
        CHECK(deck.consume_actions().held==0);                                                                                 // lifted: the fire is released
        e.type=SDL_EVENT_FINGER_DOWN; e.tfinger.fingerID=9; e.tfinger.x=1; e.tfinger.y=1; e.tfinger.timestamp=1000000000; touch.event(e,deck,true);
        CHECK((deck.latest_aim()==std::array<float,2>{.8f,.6f}));
        CHECK((deck.consume_actions().pressed&bit(uc::Action::knife))==0);                                                      // the jump to the new tap is no knife swing
        e.type=SDL_EVENT_FINGER_UP; touch.event(e,deck,true); deck.consume_actions();
        e={}; e.type=SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.button=SDL_BUTTON_RIGHT; e.button.which=SDL_TOUCH_MOUSEID;
        touch.event(e,deck,true); CHECK(deck.consume_actions().held==0);                                                        // the mouse SDL invents from a touch is dropped while playing
        e.button.which=0; touch.event(e,deck,true); CHECK(deck.consume_actions().held==bit(uc::Action::fire));                  // a real mouse is not
    }
    {   // a finger on the Steam Deck reaches the game as a cursor jump plus the left button: a sharp swipe is a knife swing, a slow drag only aims
        for(const int step_ms:{8,40}) {
            uc::NativeInput deck; deck.context(uc::InputContext::gameplay); deck.focus(true);
            uc::HostInput touch; touch.extent(1280,800); touch.pointer(uc::PointerMode::absolute); touch.touch_pointer(true);
            uint64_t t=1000000000ull;
            e={}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.x=300; e.motion.y=500; e.motion.timestamp=t; touch.event(e,deck,true);   // the cursor jumps to the finger
            e={}; e.type=SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.button=SDL_BUTTON_LEFT; touch.event(e,deck,true);
            {   // the touch is one press of the action button and a held fire: no held Interact (the knife/grenade stance), no grenade chord
                const auto frame=deck.consume_actions();
                CHECK(frame.held==bit(uc::Action::fire)); CHECK((frame.pressed&bit(uc::Action::grenade))==0);
                bool tapped=false; for(size_t i=0;i<frame.count;++i) tapped|=frame.events[i].action==uc::Action::interact && frame.events[i].down;
                CHECK(tapped);
            }
            bool knife=false;
            const int px=step_ms==8 ? 80 : 20;
            for(int i=1;i<=6;++i) {   // 6 steps: 89 px per 8 ms = 11000 px/s; 22 px per 40 ms = 560 px/s
                e={}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.x=300+px*i; e.motion.y=500-px/2*i; t+=uint64_t(step_ms)*1000000ull; e.motion.timestamp=t; touch.event(e,deck,true);
                knife|=(deck.consume_actions().pressed&bit(uc::Action::knife))!=0;
            }
            std::printf("swipe at %d ms per sample: knife=%d\n",step_ms,int(knife));
            CHECK(knife==(step_ms==8));   // 11000 px/s swings the knife; 560 px/s (below the finger's 6000) does not
        }
        uc::NativeInput deck; deck.context(uc::InputContext::gameplay); deck.focus(true);
        uc::HostInput touch; touch.extent(1280,800); touch.pointer(uc::PointerMode::absolute); touch.touch_pointer(true);
        uint64_t t=1000000000ull; bool knife=false;
        e={}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.x=300; e.motion.y=500; e.motion.timestamp=t; touch.event(e,deck,true);
        e={}; e.type=SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.button=SDL_BUTTON_LEFT; touch.event(e,deck,true);
        for(int i=1;i<=40;++i) {   // a slow drag: 400 px in 640 ms
            e={}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.x=300+10*i; e.motion.y=500; t+=16000000ull; e.motion.timestamp=t; touch.event(e,deck,true);
            knife|=(deck.consume_actions().pressed&bit(uc::Action::knife))!=0;
        }
        CHECK(!knife);
    }
    {   // the cursor (the Deck's trackpad) after the gyro: its steps move the pointer from where the gyro left it, not back to where the cursor was
        uc::NativeInput deck; deck.context(uc::InputContext::gameplay); deck.focus(true);
        uc::HostInput pad; pad.extent(1000,500); pad.pointer(uc::PointerMode::absolute); pad.touch_pointer(true);
        const auto motion=[&](float x,float y) { e={}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.x=x; e.motion.y=y; pad.event(e,deck,true); };
        motion(500,250); CHECK((deck.latest_aim()==std::array<float,2>{0,0}));         // the first point is only the reference
        motion(600,250); CHECK(std::abs(deck.latest_aim()[0]-.16f)<1e-4f);            // 100 px right: a fifth of the window, at the pointer's .8 edge
        deck.move_raw(100,0,1000,500);                                                 // the gyro moves the pointer; the cursor stays
        const float after_gyro=deck.latest_aim()[0];
        motion(610,250); CHECK(std::abs(deck.latest_aim()[0]-(after_gyro+.016f))<1e-4f);   // 10 px of the cursor: 10 px more, no jump back to the cursor's own place
        e={}; e.type=SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.button=SDL_BUTTON_LEFT; pad.event(e,deck,true);   // a touch puts the pointer under the finger (the cursor is there)
        CHECK(std::abs(deck.latest_aim()[0]-(2*610.f/1000-1)*.8f)<1e-4f);
    }
    {   // in the game's menus a touch is Confirm (the right mouse button would be Back there), in a 3D scene it fires
        uc::NativeInput menu; menu.context(uc::InputContext::menu); menu.focus(true);
        uc::HostInput touch; touch.extent(1280,800); touch.pointer(uc::PointerMode::absolute); touch.touch_pointer(true);
        e={}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.x=640; e.motion.y=400; touch.event(e,menu,true);
        e={}; e.type=SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.button=SDL_BUTTON_LEFT; touch.event(e,menu,true);
        auto frame=menu.consume_actions();
        CHECK(frame.held==0);   // the menu sees the pointer first: Confirm comes a moment later, so the first tap activates the item
        e.type=SDL_EVENT_MOUSE_BUTTON_UP; touch.event(e,menu,true);   // a quick tap: the finger is already up
        const uint64_t now=SDL_GetTicksNS();
        touch.update(menu,now+10000000ull); CHECK(menu.consume_actions().held==0);
        touch.update(menu,now+100000000ull); frame=menu.consume_actions();
        CHECK((frame.pressed&bit(uc::Action::confirm))!=0); CHECK((frame.pressed&bit(uc::Action::back))==0);
        touch.update(menu,now+150000000ull); CHECK((menu.consume_actions().held&bit(uc::Action::confirm))!=0);   // held a little while for the game to see it
        touch.update(menu,now+300000000ull); frame=menu.consume_actions(); CHECK(frame.held==0);
        menu.context(uc::InputContext::gameplay);   // the same finger now fires: the touch decides by the context it began in
        e.type=SDL_EVENT_MOUSE_BUTTON_DOWN; touch.event(e,menu,true);
        CHECK(menu.consume_actions().held==bit(uc::Action::fire));
    }
}
