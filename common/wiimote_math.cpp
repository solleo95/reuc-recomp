#include "wiimote_math.h"
#include <algorithm>
#include <cmath>
namespace uc {
namespace {
constexpr float kCountsPerG=26.f,kSwingCountsPerG=150.f,kGravitySeconds=.25f,kStickRange=95.f,kMinSeparation=6.f;
constexpr float kCenterX=512.f,kCenterY=384.f;   // the camera sees 1024x768
}
std::optional<std::array<float,2>> wii_ir_pointer(const std::array<WiiIrDot,4>& dots) {
    const WiiIrDot* first=nullptr;const WiiIrDot* second=nullptr;float widest=0;
    for(size_t i=0;i<dots.size();++i) for(size_t j=i+1;j<dots.size();++j) {
        if(!dots[i].valid || !dots[j].valid) continue;
        const float distance=std::hypot(float(dots[i].x)-dots[j].x,float(dots[i].y)-dots[j].y);
        if(distance>widest) {widest=distance;first=&dots[i];second=&dots[j];}
    }
    if(!first || widest<kMinSeparation) return std::nullopt;
    if(first->x>second->x) std::swap(first,second);   // left to right in the camera's picture
    const float roll=std::atan2(float(second->y)-first->y,float(second->x)-first->x);
    const float vx=(float(first->x)+second->x)/2-kCenterX,vy=(float(first->y)+second->y)/2-kCenterY;
    const float turned_x=vx*std::cos(roll)+vy*std::sin(roll),turned_y=-vx*std::sin(roll)+vy*std::cos(roll);   // the roll undone
    return std::array<float,2>{std::clamp(-turned_x/kCenterX*kWiiPointerRange,-1.f,1.f),std::clamp(-turned_y/kCenterY*kWiiPointerRange,-1.f,1.f)};   // the bar moves against the aim
}
std::array<float,2> wii_stick(std::array<uint8_t,2> raw,float dead_zone) {
    const float x=(float(raw[0])-128)/kStickRange,y=(128-float(raw[1]))/kStickRange;
    const float length=std::hypot(x,y);
    if(length<=dead_zone || length==0) return {0,0};
    const float scale=(length-dead_zone)/(1-dead_zone)/length;
    return {std::clamp(x*scale,-1.f,1.f),std::clamp(y*scale,-1.f,1.f)};
}
std::array<float,2> WiiSwing::feed(std::array<uint8_t,3> accel,uint64_t timestamp_ns,float threshold_g) {
    const std::array<float,3> now{float(accel[0]),float(accel[1]),float(accel[2])};
    const double seconds=last_ns_ && timestamp_ns>last_ns_ ? double(timestamp_ns-last_ns_)/1e9 : 0.0;
    last_ns_=timestamp_ns;
    if(!have_ || seconds==0 || seconds>.2) {have_=true;gravity_=now;return {0,0};}   // a new stream: nothing to compare with yet
    const float dx=now[0]-gravity_[0],dz=now[2]-gravity_[2];
    const float push=std::hypot(dx,dz)/kCountsPerG;
    if(push>=threshold_g) return {dx/kCountsPerG*kSwingCountsPerG,-dz/kCountsPerG*kSwingCountsPerG};   // gravity is left alone while the hand pushes
    const float follow=1-std::exp(-float(seconds)/kGravitySeconds);
    for(size_t i=0;i<3;++i) gravity_[i]+=(now[i]-gravity_[i])*follow;
    return {0,0};
}
}
