#include "check.h"
#include "native_input.h"
#include "gamepad_input.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
using namespace uc;
constexpr uint64_t kSecond=1000000000ull;
static uint64_t bit(Action a) { return uint64_t(1)<<unsigned(a); }
static bool near(float a,float b,float tolerance=2e-3f) { return std::fabs(a-b)<=tolerance; }
// A virtual SDL gamepad: the real SDL path (events, mapping, sensors) without a device.
struct Virtual {
    SDL_JoystickID id=0; SDL_Joystick* joystick=nullptr;
    explicit Virtual(const char* name="Test Pad") {
        SDL_VirtualJoystickDesc desc; SDL_INIT_INTERFACE(&desc);
        const SDL_VirtualJoystickSensorDesc sensors[1]={{SDL_SENSOR_GYRO,250.f}};
        desc.type=SDL_JOYSTICK_TYPE_GAMEPAD; desc.naxes=SDL_GAMEPAD_AXIS_COUNT; desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
        desc.nsensors=1; desc.sensors=sensors; desc.name=name;
        id=SDL_AttachVirtualJoystick(&desc); CHECK(id);
        joystick=SDL_OpenJoystick(id); CHECK(joystick);
    }
    ~Virtual() { detach(); }
    void detach() { if(!joystick)return; SDL_CloseJoystick(joystick); joystick=nullptr; SDL_DetachVirtualJoystick(id); }
    void button(SDL_GamepadButton b,bool down) { CHECK(SDL_SetJoystickVirtualButton(joystick,b,down)); }
    void axis(SDL_GamepadAxis a,int value) { CHECK(SDL_SetJoystickVirtualAxis(joystick,a,Sint16(value))); }
    void trigger(SDL_GamepadAxis a,float pull) { axis(a,int(std::lround(pull*65535.f))-32768); }   // joysticks report triggers from -32768 (released) to 32767; the gamepad layer maps that to 0..32767
    void gyro(float yaw,float pitch,uint64_t timestamp_ns) { const float data[3]={pitch,yaw,0}; CHECK(SDL_SendJoystickVirtualSensorData(joystick,SDL_SENSOR_GYRO,timestamp_ns,data,3)); }
};
static void pump(GamepadInput& pads,NativeInput* input,bool active=true) {
    SDL_PumpEvents();
    SDL_Event event; while(SDL_PollEvent(&event)) pads.event(event,input);
    if(input)pads.update(*input,active);
}
struct Rig {
    GamepadInput pads; NativeInput input; uint64_t now=kSecond;
    Rig() { input.extent(1280,720); input.focus(true); input.context(InputContext::gameplay); input.consume_at(now); }
    void pump(bool active=true) { ::pump(pads,&input,active); }
    InputSample run(double seconds) { InputSample last; for(int i=0;i<int(seconds*20+.5);++i) {now+=kSecond/20; pump(); last=input.consume_at(now);} return last; }
};
static void family_from_type_table() {
    CHECK(pad_family_from_type(SDL_GAMEPAD_TYPE_XBOX360)==PadFamily::xbox && pad_family_from_type(SDL_GAMEPAD_TYPE_XBOXONE)==PadFamily::xbox);
    CHECK(pad_family_from_type(SDL_GAMEPAD_TYPE_PS3)==PadFamily::playstation && pad_family_from_type(SDL_GAMEPAD_TYPE_PS4)==PadFamily::playstation && pad_family_from_type(SDL_GAMEPAD_TYPE_PS5)==PadFamily::playstation);
    CHECK(pad_family_from_type(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO)==PadFamily::nintendo && pad_family_from_type(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR)==PadFamily::nintendo);
    // A pad SDL cannot name (a GameSir or another XInput clone reports a standard or unknown type) has the Xbox layout and look: that is what almost every PC pad is.
    CHECK(pad_family_from_type(SDL_GAMEPAD_TYPE_STANDARD)==PadFamily::xbox && pad_family_from_type(SDL_GAMEPAD_TYPE_UNKNOWN)==PadFamily::xbox);
}
static void a_pad_appears_and_its_buttons_reach_the_input() {
    Rig rig; CHECK(!rig.pads.status().connected);
    Virtual pad("Virtual Test Pad"); rig.pump();
    const auto status=rig.pads.status(); CHECK(status.connected && status.name=="Virtual Test Pad" && status.has_gyro);
    pad.button(SDL_GAMEPAD_BUTTON_WEST,true); rig.pump();
    CHECK(rig.input.consume_actions().pressed==bit(Action::reload));                     // X reloads
    pad.button(SDL_GAMEPAD_BUTTON_WEST,false); rig.pump(); rig.input.consume_actions();
    pad.button(SDL_GAMEPAD_BUTTON_SOUTH,true); rig.pump();
    CHECK(rig.input.consume_actions().held&bit(Action::interact));
}
static void triggers_press_at_half_with_a_hysteresis() {
    Rig rig; Virtual pad; rig.pump();
    const auto fire=[&]{ return (rig.input.consume_actions().held&bit(Action::fire))!=0; };
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,.37f); rig.pump(); CHECK(!fire());
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,.49f); rig.pump(); CHECK(!fire());        // not yet
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,.52f); rig.pump(); CHECK(fire());         // pressed
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,.43f); rig.pump(); CHECK(fire());         // held by the hysteresis
    pad.trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,.37f); rig.pump(); CHECK(!fire());        // released
}
static void sticks_move_the_pointer_and_the_camera() {
    Rig rig; Virtual pad; rig.pump();
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX,32767); rig.pump();
    CHECK(near(rig.run(1).x,1.f));                                                       // the left stick aims: past the edge, clamped
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX,0); rig.pump(); rig.input.aim(0,0);
    pad.axis(SDL_GAMEPAD_AXIS_RIGHTX,32767); rig.pump();
    const auto after=rig.run(.5);
    CHECK(after.x==0 && rig.input.consume_actions().camera[0]==127);                     // the right stick is the camera, the pointer stays
    pad.axis(SDL_GAMEPAD_AXIS_RIGHTX,0); pad.axis(SDL_GAMEPAD_AXIS_RIGHTY,-32768); rig.pump();
    CHECK(rig.input.consume_actions().camera[1]==127);                                   // up is positive
}
static void gyro_is_calibrated_and_moves_the_pointer() {
    Rig rig; Virtual pad; rig.pump();
    GamepadSettings settings; settings.gyro.mode=GyroMode::always; rig.input.pad_settings(settings);
    uint64_t t=kSecond;
    for(int i=0;i<130;++i) {pad.gyro(.02f,-.01f,t); t+=4000000; rig.pump();}             // resting offset while the half second of measuring passes
    const auto rest=rig.input.consume_at(rig.now+=kSecond/20);
    for(int i=0;i<20;++i) {pad.gyro(.02f,-.01f,t); t+=4000000; rig.pump();}              // still at rest after calibration: a pad on a desk
    const auto still=rig.input.consume_at(rig.now+=kSecond/20); CHECK(still.x==rest.x && still.y==rest.y);
    for(int i=0;i<3;++i) {pad.gyro(1.02f,-.01f,t); t+=4000000; rig.pump();}              // 1 rad/s left on top of the offset for three 4 ms samples: 24 counts
    const auto moved=rig.input.consume_at(rig.now+=kSecond/20);
    CHECK(near(moved.x-still.x,-2*24.f/1280,1e-3f) && near(moved.y,still.y,1e-3f));
}
static void a_removed_pad_is_released() {
    Rig rig; auto pad=std::make_unique<Virtual>(); rig.pump();
    pad->axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767); pad->axis(SDL_GAMEPAD_AXIS_LEFTX,32767); pad->axis(SDL_GAMEPAD_AXIS_RIGHTX,32767); pad->button(SDL_GAMEPAD_BUTTON_SOUTH,true);
    rig.pump(); CHECK(rig.input.consume_actions().held&bit(Action::interact));
    pad.reset(); rig.pump(); rig.pump();                                                 // unplugged with everything held (SDL reports the removal on its next update)
    const auto frame=rig.input.consume_actions();
    CHECK(!rig.pads.status().connected);
    CHECK(frame.camera[0]==0);
    CHECK(frame.held==0);
    const auto at=rig.input.consume_at(rig.now+=kSecond/20); CHECK(rig.run(.5).x==at.x);   // the pointer is still
}
static void an_inactive_gamepad_feeds_nothing_and_resumes_cleanly() {
    Rig rig; Virtual pad; rig.pump();
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX,32767); pad.button(SDL_GAMEPAD_BUTTON_WEST,true); rig.pump();
    rig.input.consume_actions();                                                         // the reload pressed while active
    rig.pump(false);                                                                     // a menu opens with the stick pushed
    const auto frozen=rig.input.consume_at(rig.now+=kSecond/20);
    for(int i=0;i<20;++i) {rig.now+=kSecond/20; rig.pump(false);}
    CHECK(rig.input.consume_at(rig.now).x==frozen.x);
    pad.button(SDL_GAMEPAD_BUTTON_NORTH,true); rig.pump(false);                         // a button pressed behind the menu...
    rig.pump(true); CHECK(rig.input.consume_actions().held==0);                          // ...is not a press when the game returns, and neither is the one still held
    CHECK(rig.run(.3).x>frozen.x);                                                       // the stick still pushed continues to aim
}
static void the_capture_sink_sees_every_input_even_while_inactive() {
    Rig rig; Virtual pad; rig.pump();
    std::vector<std::pair<PhysicalInput,bool>> seen;
    rig.pads.capture_sink([&](PhysicalInput input,bool down){ seen.emplace_back(input,down); });
    pad.button(SDL_GAMEPAD_BUTTON_EAST,true); pad.axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767); pad.axis(SDL_GAMEPAD_AXIS_LEFTY,-32768); rig.pump(false);
    const auto saw=[&](PhysicalInput input) { return std::count(seen.begin(),seen.end(),std::make_pair(input,true))==1; };
    CHECK(seen.size()==3 && saw({PhysicalKind::pad_button,1}) && saw({PhysicalKind::pad_axis,11}) && saw({PhysicalKind::pad_axis,2}));   // east, the right trigger, and the left stick up (axis 1 negative: code 1*2+0)
    CHECK(rig.input.consume_actions().held==0);
}
int main() {
    // Only the virtual pads of this test: a gamepad plugged into the PC must not take part.
    for(const char* hint:{SDL_HINT_JOYSTICK_HIDAPI,SDL_HINT_JOYSTICK_RAWINPUT,SDL_HINT_JOYSTICK_WGI,SDL_HINT_JOYSTICK_DIRECTINPUT,SDL_HINT_XINPUT_ENABLED}) SDL_SetHint(hint,"0");
    family_from_type_table(); a_pad_appears_and_its_buttons_reach_the_input(); triggers_press_at_half_with_a_hysteresis();
    sticks_move_the_pointer_and_the_camera(); gyro_is_calibrated_and_moves_the_pointer(); a_removed_pad_is_released();
    an_inactive_gamepad_feeds_nothing_and_resumes_cleanly(); the_capture_sink_sees_every_input_even_while_inactive();
}
