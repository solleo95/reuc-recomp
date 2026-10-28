#include "gamepad_math.h"
#include <algorithm>
#include <cmath>
namespace uc {
namespace {
constexpr uint64_t kCalibrationNs=500000000ull;
constexpr float kCalibrationMovement=.05f,kTrackWindow=.02f,kTrackRate=.002f;   // rad/s: movement that restarts the measurement, band that follows drift, drift step per sample
// Magnitude past the dead zone as a fraction of the remaining range (0..1), 0 inside the dead zone.
float rescaled(float magnitude,float dead_zone) {
    if(magnitude<=dead_zone) return 0;
    return std::min(1.f,(magnitude-dead_zone)/(1.f-dead_zone));
}
}
std::array<float,2> stick_aim_velocity(float x,float y,const StickCurve& curve) {
    const float magnitude=std::hypot(x,y),scaled=rescaled(magnitude,curve.dead_zone);
    if(scaled==0) return {0,0};
    const float speed=scaled*scaled*curve.speed/magnitude;
    return {x*speed,y*speed};
}
std::array<int8_t,2> camera_axes(float x,float y,float dead_zone) {
    const float magnitude=std::hypot(x,y),scaled=rescaled(magnitude,dead_zone);
    if(scaled==0) return {0,0};
    const float scale=scaled*127.f/magnitude;
    return {int8_t(std::lround(x*scale)),int8_t(std::lround(y*scale))};
}
std::array<float,2> gyro_aim_counts(float yaw,float pitch,float dt,const GyroSettings& gyro) {
    if(std::hypot(yaw,pitch)<gyro.dead_zone) return {0,0};
    const float x=-yaw*dt*kGyroCountsPerRadian*gyro.sensitivity_x*(gyro.invert_x ? -1.f : 1.f);
    const float y=-pitch*dt*kGyroCountsPerRadian*gyro.sensitivity_y*(gyro.invert_y ? -1.f : 1.f);
    return {x,y};
}
std::array<float,2> GyroBias::filter(float yaw,float pitch,uint64_t timestamp_ns) {
    if(!calibrated_) {
        const bool restart=samples_==0 || timestamp_ns<start_ns_ ||
            std::hypot(yaw-float(sum_yaw_/samples_),pitch-float(sum_pitch_/samples_))>kCalibrationMovement;
        if(restart) {start_ns_=timestamp_ns;samples_=0;sum_yaw_=sum_pitch_=0;}
        ++samples_;sum_yaw_+=yaw;sum_pitch_+=pitch;
        if(timestamp_ns-start_ns_>=kCalibrationNs) {
            bias_yaw_=float(sum_yaw_/samples_);bias_pitch_=float(sum_pitch_/samples_);calibrated_=true;
        }
        return {0,0};
    }
    if(std::hypot(yaw-bias_yaw_,pitch-bias_pitch_)<kTrackWindow) {
        bias_yaw_+=(yaw-bias_yaw_)*kTrackRate;bias_pitch_+=(pitch-bias_pitch_)*kTrackRate;
    }
    return {yaw-bias_yaw_,pitch-bias_pitch_};
}
}
