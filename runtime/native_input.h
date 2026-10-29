#pragma once
#include "ppc_rt.h"
#include "input_bindings.h"
#include "gamepad_config.h"
#include "gamepad_target.h"
#include "pointer_settings.h"
#include "wiimote_config.h"
#include "wiimote_target.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <memory>
#include <vector>

namespace uc {
constexpr uint32_t kButtonA=0x0800,kButtonB=0x0400;
struct InputSample {
    float x=0,y=0,dx=0,dy=0;
    uint32_t held=0,pressed=0,released=0;
    bool valid=false;
    float stick_x=0,stick_y=0; // Derived from camera actions after raw input/demo/network sampling.
};
struct InputFrame { InputSample sample; ActionFrame actions; uint64_t generation=0; bool host=true; };
// pad_button/pad_axis: code is the PhysicalInput code, value 0/1 (an axis press is a stick direction or trigger past half). pad_stick: code 0 left / 1 right
// stick of pad 0, x/y -1..1 (y down positive). pad_gyro: x yaw, y pitch in rad/s (yaw positive turning left, pitch positive tilting up).
// aim: the pointer at x/y -1..1 of the window, as an absolute device (a lightgun) reports it. wii_button: code 0..kWiiButtonCount-1, value 0/1. wii_pointer: the IR pointer
// x/y -1..1 (y down), smoothed like the real one. wii_stick: the nunchuk stick x/y -1..1 (y down). wii_motion: a swing as mouse counts x/y.
enum class HostEventKind:uint8_t { key,mouse,wheel,motion,focus,pad_button,pad_axis,pad_stick,pad_gyro,aim,wii_button,wii_pointer,wii_stick,wii_motion };
struct HostEvent { HostEventKind kind; uint32_t code; int value; float x=0,y=0; };
using InputProbe=void(*)(const ppc::Ctx&,const InputSample&);
void input_hle_set_probe(InputProbe probe);
using InputMap=InputSample(*)(const ppc::Ctx&,const InputSample&);
void input_hle_set_map(InputMap map);
class NativeInput : public GamepadTarget, public WiimoteTarget {
public:
    NativeInput();
    void focus(bool focused);
    void bindings(const Bindings&);
    void context(InputContext);
    void qte_window(bool);
    void host_button(PhysicalInput,bool) override;
    void host_wheel(int);
    void host_motion(float,float,uint64_t timestamp_ns=0);
    void host_camera(std::array<int8_t,2>);   // the analog camera stick (-127..127, x right, y up); it stays until changed, released by focus loss (the next stick update replaces it)
    void extent(uint32_t width,uint32_t height);   // the window in pixels: gyro counts and stick speed are in pixels
    void pad_settings(const GamepadSettings&);     // roles, curves, gyro, gamepad bindings; throws std::invalid_argument for an invalid set
    // The aim stick moves the pointer while deflected, the camera stick is the analog camera; the gyro moves the pointer and feeds the knife/shake detectors per GyroMode.
    void host_pad_stick(unsigned pad,PadSide,float x,float y) override;
    void host_pad_gyro(float yaw_rate,float pitch_rate,uint64_t timestamp_ns) override;
    void pad_reset() override;   // the keyboard and mouse are untouched
    // The Wii Remote: the IR pointer (smoothed, never a swing), the nunchuk stick as the analog camera, swings as knife and shake input, its buttons through host_button.
    void wiimote_settings(const WiimoteSettings&);   // smoothing and the binding table; throws std::invalid_argument for an invalid set
    void host_wii_pointer(float x,float y,uint64_t timestamp_ns) override;
    void host_wii_stick(float x,float y) override;
    void host_wii_motion(float dx,float dy,uint64_t timestamp_ns) override;
    void wii_reset() override;
    ActionFrame consume_actions();
    ActionFrame consume_service_actions();
    bool move_raw(float dx,float dy,uint32_t width,uint32_t height);
    bool aim(float x,float y);
    // The pointer from a position in the window (pixels, y down): a lightgun or another absolute device. The step from the previous position feeds the knife and
    // shake detectors like a mouse motion; the first position after a focus change is taken as it is.
    // gesture_scale turns the pixels of a step into the counts the detectors are tuned for (a mouse's): 1/density on a scaled display.
    bool aim_absolute(float px,float py,uint32_t width,uint32_t height,uint64_t timestamp_ns=0,float gesture_scale=1);
    void pointer_smoothing(float fraction);   // 0..0.9: the share of the pointer's old place that stays at each new absolute position
    // A shot (`fire`) while the pointer is in the corner / at the edge of the window, `size_fraction` (0..0.5) of its width and height, reloads instead: the game sees
    // `reload` for one frame and never sees that shot. Only in gameplay; throws std::invalid_argument for a size outside 0..0.5.
    void reload_zone(ReloadZone,float size_fraction);
    bool apply_test_command(uint64_t sequence,float x,float y,uint32_t held);
    bool apply_test_event(uint64_t sequence,const HostEvent&,uint32_t width,uint32_t height);
    bool using_host_input();
    uint64_t generation();
    uint64_t last_consumed_sequence();
    void button(uint32_t mask,bool down);
    InputSample consume();
    InputSample consume_at(uint64_t now_ns);   // consume() with the clock given (the stick moves the pointer by the time since the previous call): tests
    InputFrame capture_frame();
    void install_frame(const InputFrame&);
    struct AimPoint { int64_t ns=0; float x=0,y=0; };
    struct Snapshot;
    std::shared_ptr<const Snapshot> checkpoint();
    void restore(const Snapshot&);
    std::array<float,2> latest_aim();     // any thread: the pointer as of now
    std::array<float,2> aim_ago(double seconds);   // any thread: the pointer `seconds` ago (the last second is kept), interpolated between mouse events
    std::array<float,2> consumed_aim();   // the pointer of the last sample the guest read
    // Demo (determinism): every value the guest reads from this object (consume, consume_actions, using_host_input, generation) is appended to one
    // ordered file while recording and returned from that file while playing, so a deterministic run needs neither the harness nor a window.
    void demo_record(const std::filesystem::path&);
    void demo_play(const std::filesystem::path&);
    bool demo_playing() const { return replaying_; }
    uint32_t demo_speed(uint32_t live);   // clock speed factor the game pacing uses in a deterministic run: recorded when it changes, replayed at the same pacing call
    ~NativeInput();
private:
    enum class Tag:uint8_t { sample=1,actions,host,generation,speed,camera };   // camera: 2 int8 after an actions record, written only while not (0,0); older demos have none
    void demo_put(Tag,const void*,size_t);
    // Playing: the i-th consume() returns the i-th recorded sample and the i-th consume_actions() the i-th recorded action frame, whatever order the
    // guest's reads interleave in (a build that schedules guest threads differently still gets the same input at the same frame); using_host_input()
    // and generation() hold the values recorded after the sample just returned.
    struct DemoActions { uint64_t held,pressed,released,generation; std::vector<ActionEvent> events; std::array<int8_t,2> camera{}; };
    struct DemoSample { InputSample sample; uint8_t host; uint64_t generation; };
    std::FILE* demo_out_=nullptr;
    std::vector<DemoSample> demo_samples_;
    std::vector<DemoActions> demo_actions_;
    size_t demo_sample_at_=0,demo_action_at_=0;
    uint8_t demo_host_=1;
    uint64_t demo_generation_=0;
    std::vector<std::pair<uint64_t,uint32_t>> demo_speeds_;   // (pacing call number, speed factor from then on)
    size_t demo_speed_at_=0;
    uint64_t speed_calls_=0;
    uint32_t speed_now_=1,speed_last_=0;
    bool replaying_=false;
    void cancel_host(); // Requires mutex_.
    void install_bindings();   // Requires mutex_: the keyboard table and the gamepad table as the resolvers' one table.
    void update_camera();      // Requires mutex_: the analog camera from the sticks with the camera role.
    void step_pointer(uint64_t now_ns);   // Requires mutex_: the aim stick moves the pointer for the time since the previous step.
    void gyro_motion(float yaw,float pitch,uint64_t timestamp_ns);   // Requires mutex_.
    Bindings keyboard_=default_bindings();
    GamepadSettings pad_;
    WiimoteSettings wii_;
    std::array<float,2> wii_stick_{};   // the nunchuk stick, -1..1, y down positive
    bool have_wii_pointer_=false;
    std::array<std::array<std::array<float,2>,2>,kPadSlots> sticks_{};   // [pad][side][x,y]
    uint64_t step_ns_=0,gyro_ns_=0;
    uint32_t width_=1280,height_=720;
    std::mutex mutex_;
    BindingResolver actions_,services_;
    InputSample state_;
    std::array<int8_t,2> camera_{};
    std::array<AimPoint,1024> trail_{};   // recent pointer positions, a ring ordered by time: aim_ago()
    size_t trail_head_=0;
    void note_aim();
    float previous_x_=0,previous_y_=0;
    float smoothing_=0,absolute_px_=0,absolute_py_=0;
    bool have_absolute_=false;
    ReloadZone zone_=ReloadZone::off;
    float zone_size_=.05f;
    bool fire_hidden_=false,reload_pulse_=false;   // a shot taken as a reload (its release is hidden too); the reload's one frame is running
    void apply_reload_zone(ActionFrame&);   // Requires mutex_.
    void set_wii_pointer(float x,float y);  // Requires mutex_ and a valid state_.
    void aim_toward(float x,float y,float smoothing,bool take_now);   // Requires mutex_ and a valid state_.
    void ease_pointer(uint64_t now_ns);                               // Requires mutex_.
    float target_x_=0,target_y_=0,target_smoothing_=0;
    bool target_on_=false;   // the pointer is still on its way to the target
    uint64_t eased_ns_=0;
    bool absolute_point(float px,float py,uint32_t width,uint32_t height,uint64_t timestamp_ns,float gesture_scale=1);   // Requires mutex_; finite px/py and a window were checked.
    uint64_t sequence_=0,host_sequence_=0,consumed_sequence_=0;
    bool legacy_test_input_=false;
    uint64_t generation_=0;
    InputContext context_=InputContext::inactive;
    std::optional<InputFrame> network_frame_;
};
void input_hle_init(NativeInput* input,NativeInput* second=nullptr);
void input_hle_set_poll(std::function<void(const ppc::Ctx&)>);
unsigned input_hle_channel(); // Guest baton owns the currently sampled channel.
void input_hle_restore_channel(unsigned channel);
std::array<float,2> input_consumed_aim();   // the pointer the guest read last for the aim channel's pad (zero without input)
std::optional<std::array<float,2>> input_consumed_aim_of(unsigned channel);   // the same for one pad; nullopt where there is none (single-player has no second)
bool input_demo_playing();   // a demo replays the recorded samples: the live pointer is not what the game consumed
bool input_aim_exact();   // true once a network session set the aim channel
unsigned input_aim_channel();   // the local player's pad (0 outside network play)
void input_hle_set_aim_channel(unsigned channel);   // network play: the local player's pad (0 host, 1 client); reset to 0 by input_hle_init
uint32_t input_demo_speed(uint32_t live);   // the clock speed factor the deterministic game pacing uses (recorded and replayed with the demo; live without input)
}
void hle_KPADRead(ppc::Ctx&);
void hle_WPADProbe(ppc::Ctx&);
