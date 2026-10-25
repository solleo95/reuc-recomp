#pragma once
#include "ppc_rt.h"
#include "input_bindings.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
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
enum class HostEventKind:uint8_t { key,mouse,wheel,motion,focus };
struct HostEvent { HostEventKind kind; uint32_t code; int value; float x=0,y=0; };
using InputProbe=void(*)(const ppc::Ctx&,const InputSample&);
void input_hle_set_probe(InputProbe probe);
using InputMap=InputSample(*)(const ppc::Ctx&,const InputSample&);
void input_hle_set_map(InputMap map);
class NativeInput {
public:
    NativeInput();
    void focus(bool focused);
    void bindings(const Bindings&);
    void context(InputContext);
    void qte_window(bool);
    void host_button(PhysicalInput,bool);
    void host_wheel(int);
    void host_motion(float,float,uint64_t timestamp_ns=0);
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
    InputFrame capture_frame();
    void install_frame(const InputFrame&);
    struct Snapshot;
    std::shared_ptr<const Snapshot> checkpoint();
    void restore(const Snapshot&);
    std::array<float,2> latest_aim();     // any thread: the pointer as of now
    std::array<float,2> consumed_aim();   // the pointer of the last sample the guest read
    // Demo (determinism): every value the guest reads from this object (consume, consume_actions, using_host_input, generation) is appended to one
    // ordered file while recording and returned from that file while playing, so a deterministic run needs neither the harness nor a window.
    void demo_record(const std::filesystem::path&);
    void demo_play(const std::filesystem::path&);
    uint32_t demo_speed(uint32_t live);   // clock speed factor the game pacing uses in a deterministic run: recorded when it changes, replayed at the same pacing call
    ~NativeInput();
private:
    enum class Tag:uint8_t { sample=1,actions,host,generation,speed };
    void demo_put(Tag,const void*,size_t);
    // Playing: the i-th consume() returns the i-th recorded sample and the i-th consume_actions() the i-th recorded action frame, whatever order the
    // guest's reads interleave in (a build that schedules guest threads differently still gets the same input at the same frame); using_host_input()
    // and generation() hold the values recorded after the sample just returned.
    struct DemoActions { uint64_t held,pressed,released,generation; std::vector<ActionEvent> events; };
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
    std::mutex mutex_;
    BindingResolver actions_,services_;
    InputSample state_;
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
std::array<float,2> input_consumed_aim();   // the pointer the guest read last (zero without input)
uint32_t input_demo_speed(uint32_t live);   // the clock speed factor the deterministic game pacing uses (recorded and replayed with the demo; live without input)
}
void hle_KPADRead(ppc::Ctx&);
void hle_WPADProbe(ppc::Ctx&);
