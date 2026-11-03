#include "host_input.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
namespace uc {
// A touchscreen (the Steam Deck): a tap puts the aim under the finger, presses the action button once (A) and fires; a held finger drags the aim along with the fire held (an
// automatic weapon keeps shooting), and does NOT hold the action button (that is the knife/grenade stance); lifting it ends the fire. A sharp swipe swings the knife (1.5 times
// the speed of a mouse swing, with no held action button). The fire is the right mouse button, the default binding of Fire (shortcut: it does not follow a rebinding of Fire).
// In the game's own menus (and any screen that is not a 3D scene) a touch is the left mouse button, Confirm: the right one is Back there.
constexpr uint64_t kMenuPressDelayNs=60000000ull,kMenuHoldNs=80000000ull;
void HostInput::touch_down(NativeInput& input) {
    touching_=true;
    touch_in_menu_=input.current_context()!=InputContext::gameplay && input.current_context()!=InputContext::qte;
    if(touch_in_menu_) {
        if(menu_press_==MenuPress::down) input.host_button({PhysicalKind::mouse,SDL_BUTTON_LEFT},false);   // the last tap's press is still down: this one needs its own edge
        menu_press_=MenuPress::waiting; menu_press_at_=SDL_GetTicksNS()+kMenuPressDelayNs; return;
    }
    input.tap(Action::interact);
    input.host_button({PhysicalKind::mouse,SDL_BUTTON_RIGHT},true);
}
void HostInput::touch_up(NativeInput& input) {
    if(!touch_in_menu_) input.host_button({PhysicalKind::mouse,SDL_BUTTON_RIGHT},false);   // in a menu update() lets go once the press has been held long enough
    input.aim_lift();   // the next tap is a fresh start, not a swing across the screen from the last one
    touching_=false;
}
void HostInput::update(NativeInput& input,uint64_t now_ns) {
    if(menu_press_==MenuPress::waiting && now_ns>=menu_press_at_) {
        input.host_button({PhysicalKind::mouse,SDL_BUTTON_LEFT},true);
        menu_press_=MenuPress::down; menu_press_at_=now_ns+kMenuHoldNs;   // menu_press_at_ is now the earliest release
    }
    if(menu_press_==MenuPress::down && !touching_ && now_ns>=menu_press_at_) {
        input.host_button({PhysicalKind::mouse,SDL_BUTTON_LEFT},false);
        menu_press_=MenuPress::none;
    }
}
bool HostInput::event(const SDL_Event& event,NativeInput& input,bool captured) {
    const bool touch_mouse=touch_pointer_ && pointer_==PointerMode::absolute;   // the finger arrives as a cursor jump plus the left button (gamescope)
    switch(event.type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST: input.focus(false); wheel_remainder_=0; touching_=false; menu_press_=MenuPress::none; break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        const auto& finger=event.tfinger;
        if(!captured || !width_ || !height_ || finger.touchID==SDL_PEN_TOUCHID) break;
        const float px=finger.x*width_,py=finger.y*height_;
        if(event.type==SDL_EVENT_FINGER_DOWN) {
            if(touching_) break;
            finger_=finger.fingerID;
            static int logged=0;
            if(logged++<5) std::fprintf(stderr,"[touch] finger down at %.2f,%.2f (touch device %llu)\n",finger.x,finger.y,(unsigned long long)finger.touchID);   // proves the display server delivers touches
            input.aim_place(px,py,width_,height_);   // the pointer lands under the finger
            touch_down(input);
        } else if(!touching_ || finger.fingerID!=finger_) break;
        else if(event.type==SDL_EVENT_FINGER_MOTION) input.aim_absolute(px,py,width_,height_,finger.timestamp,1/density_,true,true);
        else touch_up(input);
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED: input.focus(captured); break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if(event.key.repeat || event.key.scancode<=SDL_SCANCODE_UNKNOWN || event.key.scancode>=SDL_SCANCODE_COUNT)break;
        input.host_button({PhysicalKind::key,uint16_t(event.key.scancode)},event.type==SDL_EVENT_KEY_DOWN);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if(!event.button.button)break;
        if(captured && event.button.which==SDL_TOUCH_MOUSEID)break;   // the mouse SDL invents from a touch: the finger events above are the touch while playing
        if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button==SDL_BUTTON_LEFT && !captured)return true;
        if(touch_mouse && captured && event.button.button==SDL_BUTTON_LEFT) {   // the cursor jumped to the finger just before: the left button is the touch
            if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN) { if(!touching_) { input.aim_place(cursor_x_,cursor_y_,width_,height_); touch_down(input); } }   // the pointer lands under the finger
            else if(touching_) touch_up(input);
            break;
        }
        input.host_button({PhysicalKind::mouse,event.button.button},event.type==SDL_EVENT_MOUSE_BUTTON_DOWN);
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        if(!captured)break;
        const double delta=event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        if(!std::isfinite(delta) || std::abs(delta)>256) { input.focus(false); throw std::invalid_argument("invalid host wheel delta"); }
        wheel_remainder_+=delta;
        const int steps=int(wheel_remainder_);
        wheel_remainder_-=steps; input.host_wheel(steps);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if(!captured || event.motion.which==SDL_TOUCH_MOUSEID) break;
        cursor_x_=event.motion.x*density_; cursor_y_=event.motion.y*density_;
        if(pointer_==PointerMode::absolute) {
            input.aim_absolute(cursor_x_,cursor_y_,width_,height_,event.motion.timestamp,1/density_,touch_mouse && touching_,touch_pointer_);   // the Deck: the cursor's steps move the pointer from where it is (the gyro may have moved it)
            if(touch_pointer_ && !touching_ && width_ && height_) {   // the grabbed cursor stops at the window's edge and the aim with it: recentre it near the edge
                const float mx=width_*.1f,my=height_*.1f;
                if(cursor_x_<mx || cursor_x_>width_-mx || cursor_y_<my || cursor_y_>height_-my) { recentre_=true; input.aim_lift(); }
            }
        }
        else if(input.move_raw(event.motion.xrel*sensitivity_,event.motion.yrel*sensitivity_,width_,height_))
            input.host_motion(event.motion.xrel,event.motion.yrel,event.motion.timestamp);
        break;
    default: break;
    }
    return false;
}
}
