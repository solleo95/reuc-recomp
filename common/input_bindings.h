#pragma once
#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace uc {
// Knife tuning: V swings from the lower left to the upper right (screen y points down). A mouse swing is a fast mouse movement: the motion of the last kKnifeWindowNs
// (raw counts) must move at kKnifeSpeed counts per second or more and be at least kKnifeMinLength long (a floor against timer noise on single events).
constexpr float kKnifeKeyDirectionX=0.70710678f,kKnifeKeyDirectionY=-0.70710678f,kKnifeSpeed=4000.f,kKnifeMinLength=12.f;
constexpr uint64_t kKnifeWindowNs=60000000ull,kKnifeReportNs=8000000ull;
// Mouse shake (motion QTE): a step is a reversal of at least kShakeStroke counts; the mouse must have moved kShakeMinPath counts within about kShakePathSeconds
// (a slow wave is aiming) and may pause at most kShakeGapNs between reports.
constexpr double kShakeStroke=20,kShakeMinPath=24,kShakePathSeconds=.06;
constexpr uint64_t kShakeGapNs=100000000ull;
enum class Action:uint8_t { interact,fire,weapon_previous,weapon_next,reload,grenade,knife,pause,confirm,back,release_cursor,screenshot,qte_shake,camera_up,camera_down,camera_left,camera_right,menu,speed_up,recenter,count };   // menu: the in-game settings menu, a plain key (host key, never reaches the game); speed_up: hold for game clock x8, a plain key of the same kind (` by default, unbound: off); recenter: gamepad aim reset, bound in the gamepad table
enum class InputContext:uint8_t { inactive,menu,gameplay,qte };
// pad_button code: SDL_GamepadButton (0..31), except the top two: the first / second touchpad of the pad (the Steam Deck's left and right trackpads) pressed past a soft threshold
// (kPadTrackpadPress + the touchpad's index): firmer than a touch, lighter than the physical click, which is the pad's own SDL button. pad_axis code: SDL_GamepadAxis*2 + (positive direction ? 1 : 0), a stick direction or a trigger pulled past half (0..15).
// wii_button code: a WiiButton of common/wiimote_protocol.h (0..kWiiButtonCount-1): the remote's buttons, then the nunchuk's Z and C.
enum class PhysicalKind:uint8_t { key,mouse,wheel,pad_button,pad_axis,wii_button };
constexpr unsigned kWiiButtonCount=13;
constexpr uint16_t kPadTrackpadPress=30;   // pad_button 30 and 31: a soft press on touchpad 0 and 1
// Touchpad pressure (0..1) that presses, and below which the press ends (a hysteresis, so a steady finger does not flutter). Measured on a Steam Deck: a resting finger reads up to about .07,
// a medium press .1 to .25, a firm one .8 to 1 (the reading saturates), so the soft press sits just above a touch.
constexpr float kTrackpadPressOn=.10f,kTrackpadPressOff=.06f;
enum class PadFamily:uint8_t { xbox,playstation,nintendo,generic,steam_deck };   // which controller's look the prompts and button names use
struct PhysicalInput { PhysicalKind kind=PhysicalKind::key; uint16_t code=0; bool operator==(const PhysicalInput&) const=default; };
enum class BindingKind:uint8_t { physical,interact_fire,interact_motion,qte_motion };
struct Binding { BindingKind kind=BindingKind::physical; PhysicalInput input; std::vector<PhysicalInput> modifiers; bool operator==(const Binding&) const=default; };
using Bindings=std::array<std::vector<Binding>,size_t(Action::count)>;
// The settings menu key (SDL scancode) by default and when none is bound: Insert, which Mac keyboards lack, so Tab there (game/host_keys.h has the same).
#if defined(__APPLE__)
inline constexpr uint16_t kDefaultMenuKey=43;   // Tab
#else
inline constexpr uint16_t kDefaultMenuKey=73;   // Insert
#endif
Bindings default_bindings();
Bindings default_pad_bindings();   // the gamepad table: only pad_button / pad_axis inputs (and the special kinds), kept apart from the keyboard table
std::optional<std::string> validate_bindings(const Bindings&);       // keyboard and mouse table
std::optional<std::string> validate_pad_bindings(const Bindings&);   // gamepad table
Bindings default_wii_bindings();   // the Wii Remote table: only wii_button inputs (and the special kinds), in the layout of the original game
std::optional<std::string> validate_wii_bindings(const Bindings&);
// The tables as one for the resolver: per action the keyboard entries, then the gamepad entries and the Wii Remote entries that the earlier ones do not already list.
Bindings merge_bindings(const Bindings& keyboard,const Bindings& pad,const Bindings& wii={});
struct ActionEvent { Action action; bool down; float direction_x=0,direction_y=0; bool pulse=false; };
// camera: the analog camera stick (gamepad), -127..127 per axis, x right and y up positive like InputSample::stick_x/stick_y; the camera_* actions stay digital and add to it.
struct ActionFrame { uint64_t held=0,pressed=0,released=0,generation=0; std::array<ActionEvent,256> events{}; size_t count=0; std::array<int8_t,2> camera{}; };
class BindingResolver {
public:
    void replace(const Bindings&);
    void context(InputContext);
    void qte_window(bool);
    void focus(bool);
    void button(PhysicalInput,bool);
    bool down(PhysicalInput) const;   // a key, mouse button or gamepad input that is held right now (never the wheel)
    void release_pad();               // releases every held gamepad button and axis input: a gamepad went away or focus left
    void release_wii();               // the same for the Wii Remote's buttons
    void wheel(int);
    // swipe: a finger on a touchscreen. It swings the knife without Interact held (a held finger must not be the knife/grenade stance) but needs twice the speed.
    void motion(float,float,uint64_t timestamp_ns=0,bool swipe=false);
    void tap(Action action) { pulse(action); }   // one press and release of an action, whatever is held
    ActionFrame consume();
private:
    bool allowed(Action) const;
    bool held(PhysicalInput) const;
    bool matches(const Binding&,PhysicalInput) const;
    bool special(Action,BindingKind) const;
    uint64_t physical_actions() const;
    void cancel();
    void finish_motion();
    void qte_motion(float,float,uint64_t);
    void emit(Action,bool,float=0,float=0,bool=false);
    void pulse(Action,float=0,float=0);
    Bindings bindings_=default_bindings();
    InputContext context_=InputContext::inactive;
    bool focused_=false,suppress_fire_=false,qte_window_=false;
    std::bitset<832> down_,blocked_;   // one slot per physical button: keys 0..511, mouse 512..767, gamepad buttons 768..799, gamepad axes 800..815, Wii Remote buttons 816..828
    uint64_t raw_actions_=0,canceled_releases_=0;
    ActionFrame frame_;
    std::optional<size_t> motion_event_;
    double motion_x_=0,motion_y_=0;
    uint64_t stroke_time_=0,stroke_lead_=0,last_motion_time_=0;
    struct Shake {uint64_t last=0; bool stroke=false; double dx=0,dy=0,proj=0,peak=0,back_x=0,back_y=0,path=0;} shake_;
};
}
