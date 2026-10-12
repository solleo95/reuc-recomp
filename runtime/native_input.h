#pragma once
#include "ppc_rt.h"
#include <mutex>

namespace uc {
constexpr uint32_t kButtonA=0x0800,kButtonB=0x0400;
struct InputSample {
    float x=0,y=0,dx=0,dy=0;
    uint32_t held=0,pressed=0,released=0;
    bool valid=false;
};
class NativeInput {
public:
    void focus(bool focused);
    bool move_raw(float dx,float dy,uint32_t width,uint32_t height);
    void button(uint32_t mask,bool down);
    InputSample consume();
private:
    std::mutex mutex_;
    InputSample state_;
    float previous_x_=0,previous_y_=0;
};
void input_hle_init(NativeInput* input);
}
void hle_KPADRead(ppc::Ctx&);
void hle_WPADProbe(ppc::Ctx&);
