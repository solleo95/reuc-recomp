#pragma once
#include "ppc_rt.h"
#include "input_bindings.h"
#include "gamepad_config.h"
#include "gamepad_target.h"
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
enum class HostEventKind:uint8_t { key,mouse,wheel,motion,focus,pad_button,pad_axis,pad_stick,pad_gyro };
struct HostEvent { HostEventKind kind; uint32_t code; int value; float x=0,y=0; };
using InputProbe=void(*)(const ppc::Ctx&,const InputSample&);
void input_hle_set_probe(InputProbe probe);
using InputMap=InputSample(*)(const ppc::Ctx&,const InputSample&);
void input_hle_set_map(InputMap map);
class NativeInput : public GamepadTarget {
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
    ActionFrame consume_actions();
    ActionFrame consume_service_actions();
    bool move_raw(float dx,float dy,uint32_t width,uint32_t height);
    bool aim(float x,float y);
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
bool input_aim_exact();   // true once a network session set the aim channel
unsigned input_aim_channel();   // the local player's pad (0 outside network play)
void input_hle_set_aim_channel(unsigned channel);   // network play: the local player's pad (0 host, 1 client); reset to 0 by input_hle_init
uint32_t input_demo_speed(uint32_t live);   // the clock speed factor the deterministic game pacing uses (recorded and replayed with the demo; live without input)
}
void hle_KPADRead(ppc::Ctx&);
void hle_WPADProbe(ppc::Ctx&);
