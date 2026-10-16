#pragma once
#include "ppc_rt.h"
#include "input_bindings.h"
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <vector>

namespace uc {
constexpr uint32_t kButtonA=0x0800,kButtonB=0x0400;
struct InputSample {
    float x=0,y=0,dx=0,dy=0;
    uint32_t held=0,pressed=0,released=0;
    bool valid=false;
};
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
    // Demo (determinism): every value the guest reads from this object (consume, consume_actions, using_host_input, generation) is appended to one
    // ordered file while recording and returned from that file while playing, so a deterministic run needs neither the harness nor a window.
    void demo_record(const std::filesystem::path&);
    void demo_play(const std::filesystem::path&);
    ~NativeInput();
private:
    enum class Tag:uint8_t { sample=1,actions,host,generation };
    void demo_put(Tag,const void*,size_t);
    bool demo_get(Tag,void*,size_t);   // false once the demo is exhausted
    std::FILE* demo_out_=nullptr;
    std::vector<uint8_t> demo_in_;
    size_t demo_at_=0;
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
};
void input_hle_init(NativeInput* input);
}
void hle_KPADRead(ppc::Ctx&);
void hle_WPADProbe(ppc::Ctx&);
