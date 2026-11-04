#include "coop_session.h"
#include "native_input.h"
#include <cmath>
#include <functional>
namespace uc {
CoopSession::CoopSession(Parts parts,bool enabled)
    : first_(parts.first),second_input_(parts.second),gamepad_(parts.gamepad),remotes_(parts.remotes),pointers_(parts.pointers),enabled_(enabled) {
    remote_target_={&first_,&first_};
    if(enabled_ && gamepad_) gamepad_->press_sink([this](SDL_JoystickID id) { pressed(DeviceId{DeviceKind::gamepad,id}); });
}
CoopSession::~CoopSession() {
    if(enabled_ && gamepad_) gamepad_->press_sink(nullptr);
    if(second_ && second_->kind==DeviceKind::pointer && pointers_) pointers_->capture(second_->id,false);   // the grab goes with the session
}
void CoopSession::sensitivity(uint32_t percent) { first_host_.sensitivity(percent); second_host_.sensitivity(percent); }
DeviceId CoopSession::remote_id(size_t reader) const {
    return {DeviceKind::wiimote,remotes_[reader] ? std::hash<std::string>{}(remotes_[reader]->device_id()) : 0};
}
void CoopSession::pressed(DeviceId device) {
    if(!enabled_ || device.kind==DeviceKind::keyboard || roster_.known(device)) return;
    if(frame_.in_gameplay && !two_player_mode_) return;   // the game takes no second player in the middle of play: an unknown device just feeds the first player. A game that was started with a second player is a two-player game: one may come back
    if(pending_) return;             // one question at a time
    if(!roster_.has_first() && !keyboard_used_) { roster_.press(device,false); return; }   // the first device used is the first player's
    pending_=device;
    if(device.kind==DeviceKind::gamepad && gamepad_) gamepad_->mute(SDL_JoystickID(device.id));   // until it is answered the new pad plays for nobody (a remote: see update_remotes)
}
void CoopSession::unmute_pending() {
    if(pending_ && pending_->kind==DeviceKind::gamepad && gamepad_) gamepad_->mute(0);
}
CoopSession::NewDevice CoopSession::pending() const {
    if(!pending_) return {};
    return {true,second_.has_value(),pending_->kind};
}
void CoopSession::answer_first() {
    if(!pending_) return;
    unmute_pending();
    roster_.press(*pending_,false);   // not in gameplay: the device becomes the first player's
    pending_.reset();
}
void CoopSession::answer_second() {
    if(!pending_) return;
    const DeviceId device=*pending_;
    unmute_pending();
    pending_.reset();
    if(second_) release_second();                               // replaced: its device goes back to the first player
    if(roster_.press(device,true)) join(device);                // an unknown device with no second player: it joins
}
void CoopSession::cancel_pending() { unmute_pending(); pending_.reset(); }
void CoopSession::join(DeviceId device) {
    second_=device;
    switch(device.kind) {
    case DeviceKind::gamepad: if(gamepad_) gamepad_->assign_second(SDL_JoystickID(device.id)); break;
    case DeviceKind::wiimote:
        for(size_t i=0;i<remotes_.size();++i) if(remotes_[i] && remote_id(i)==device) { second_remote_=i; remotes_[i]->set_player(1); }
        break;
    case DeviceKind::pointer:
        if(pointers_) pointers_->capture(device.id,true);
        {   // SDL delivered the join click to the first player (the device was not grabbed yet) and its release will never come: what the first player holds is let go, and SDL's copies of the click are ignored
            const bool focused=first_.focused();
            first_.focus(false); first_.focus(focused);
            swallow_until_=frame_.now_ns+150000000ull;
        }
        break;
    case DeviceKind::keyboard: break;
    }
    two_player_mode_=true;
    input_hle_set_second_connected(true);   // the game sees the second pad from now on, and keeps seeing it (see the spec: leaving mid-mission is not announced)
}
void CoopSession::leave() {
    if(!second_) return;
    const DeviceId device=*second_;
    second_.reset();
    switch(device.kind) {
    case DeviceKind::gamepad: if(gamepad_) gamepad_->assign_second(0); break;
    case DeviceKind::wiimote: if(second_remote_<remotes_.size() && remotes_[second_remote_]) remotes_[second_remote_]->set_player(0); break;   // its target goes back in update_remotes
    case DeviceKind::pointer: if(pointers_) pointers_->capture(device.id,false); break;   // also for a device that went away: the backend forgets it
    case DeviceKind::keyboard: break;
    }
    second_input_.pad_reset(); second_input_.wii_reset();
    const bool focused=second_input_.focused();
    second_input_.focus(false); second_input_.focus(focused);   // whatever the second player held (a mouse button, a key) is let go; a menu's or a pause's focus stays as it is
}
void CoopSession::release_second() {
    if(!enabled_ || !second_) return;
    roster_.release_second();
    leave();
    if(!frame_.game_in_play) { two_player_mode_=false; input_hle_set_second_connected(false); }   // a menu of the game: a one-player game again
}
bool CoopSession::busier_pointer(uint64_t id) const {
    const auto own=motion_.find(id);
    const uint64_t mine=own==motion_.end() ? 0 : own->second;
    for(const auto& [other,moved]:motion_) if(other!=id && moved>mine) return true;
    return false;
}
void CoopSession::pointer_pressed(DeviceId device) {
    if(!enabled_) return;
    if(keyboard_used_ && !roster_.known(device) && !busier_pointer(device.id)) { roster_.press(device,false); return; }   // a keyboard player aims with a mouse: the busiest pointer (or the only one) is theirs
    pressed(device);
}
void CoopSession::sdl_mouse(const SDL_Event& event) {
    if(!enabled_) return;
    const bool motion=event.type==SDL_EVENT_MOUSE_MOTION;
    if(!motion && event.type!=SDL_EVENT_MOUSE_BUTTON_DOWN) return;
    const SDL_MouseID which=motion ? event.motion.which : event.button.which;
    if(which==0 || which==SDL_TOUCH_MOUSEID || which==SDL_PEN_MOUSEID) return;   // the global cursor, a touch, a pen: no device of its own
    if(motion) motion_[which]+=uint64_t(std::ceil(std::fabs(event.motion.xrel)+std::fabs(event.motion.yrel)));
    else pointer_pressed(DeviceId{DeviceKind::pointer,which});
}
void CoopSession::tick(const Frame& frame) {
    if(!enabled_) return;
    frame_=frame;
    if(pointers_) pointers_->hint(frame.pointer_absolute,frame.in_gameplay);
    if(second_ && second_->kind==DeviceKind::gamepad && gamepad_ && gamepad_->second()!=SDL_JoystickID(second_->id)) {   // its pad was unplugged
        roster_.remove(*second_); leave();
    }
    if(!pointers_) return;
    std::vector<PointerEvent> events; std::vector<uint64_t> removed;
    pointers_->poll(events,removed);
    for(const auto& event:events) {
        const DeviceId device{DeviceKind::pointer,event.device};
        if(event.kind==PointerEvent::Kind::relative) motion_[event.device]+=uint64_t(std::ceil(std::fabs(event.x)+std::fabs(event.y)));
        else if(event.kind==PointerEvent::Kind::absolute || event.kind==PointerEvent::Kind::wheel) ++motion_[event.device];
        if(event.kind==PointerEvent::Kind::button && event.down) pointer_pressed(device);   // a press, never motion, joins
        if(roster_.route(device)==Player::two) feed_pointer_event(second_host_,second_input_,event,frame.window,frame.captured);
        else if(pointers_->all_devices()) feed_pointer_event(first_host_,first_,event,frame.window,frame.captured);   // otherwise SDL carries the first player's mouse
    }
    for(const uint64_t id:removed) {
        if(pending_ && *pending_==DeviceId{DeviceKind::pointer,id}) cancel_pending();   // the question is about a device that is gone
        if(roster_.remove(DeviceId{DeviceKind::pointer,id})) leave();
    }
}
void CoopSession::update_remotes(bool active,const WiimoteSettings& settings) {
    for(size_t i=0;i<remotes_.size();++i) {
        auto* remote=remotes_[i];
        if(!remote) continue;
        WiimoteSettings reader_settings=settings;
        if(i==1 && remotes_[0] && remotes_[0]->device_id().empty() && remote->device_id().empty()) reader_settings.enabled=false;   // the second reader looks for a remote only once the first has one: two readers racing for one remote could leave the first player's menu and bindings window without it
        if(enabled_) {
            const bool connected=!remote->device_id().empty();
            if(connected && remote->pending_press()) pressed(remote_id(i));
            if(!connected && second_ && second_->kind==DeviceKind::wiimote && second_remote_==i) { roster_.remove(*second_); leave(); }   // the second player's remote went away
        }
        NativeInput* want=second_ && second_->kind==DeviceKind::wiimote && second_remote_==i ? &second_input_ : &first_;
        if(pending_ && pending_->kind==DeviceKind::wiimote && remote_id(i)==*pending_) want=nullptr;   // a remote a question is open about plays for nobody until it is answered
        if(want!=remote_target_[i]) { if(remote_target_[i]) remote->release(*remote_target_[i]); remote_target_[i]=want; }
        remote->update(want,active,reader_settings);
    }
}
bool CoopSession::swallows_mouse(const SDL_Event& event) const {
    if(!enabled_ || !pointers_) return false;
    const bool mouse=event.type==SDL_EVENT_MOUSE_MOTION || event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_UP || event.type==SDL_EVENT_MOUSE_WHEEL;
    if(!mouse) return false;
    if(pointers_->all_devices()) return true;   // every mouse reaches the game through the backend
    return swallow_until_ && event.common.timestamp<=swallow_until_;   // SDL's late copies of the join click
}
CoopSession::Status CoopSession::status() const {
    Status status;
    status.joined=second_.has_value();
    if(second_) status.kind=second_->kind;
    if(pointers_) status.problem=pointers_->status();
    return status;
}
}
