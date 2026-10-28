#pragma once
#include <array>
#include <cstdint>
namespace uc {
enum class GyroMode:uint8_t { off,always,hold };
struct StickCurve { float dead_zone=.15f,speed=1.2f; bool operator==(const StickCurve&) const=default; };   // speed: screen heights per second at full deflection
// dead_zone is in rad/s (half a degree per second): below it a controller lying on a desk does not move the pointer.
struct GyroSettings { GyroMode mode=GyroMode::off; float sensitivity_x=1,sensitivity_y=1,dead_zone=.0087f; bool invert_x=false,invert_y=false; bool operator==(const GyroSettings&) const=default; };
// Mouse counts per radian of rotation. A hand swing of the controller (a few rad/s) must reach kKnifeSpeed (4000 counts/s) and a careful aim (under 1 rad/s) must not.
constexpr float kGyroCountsPerRadian=2000.f;
// Pointer velocity in screen heights per second from a stick (y down is positive, as SDL reports it): radial dead zone, rescaled, then a quadratic curve.
std::array<float,2> stick_aim_velocity(float x,float y,const StickCurve&);
// Analog camera from a stick: radial dead zone, rescaled to the unit circle, as -127..127 (orientation of the input kept).
std::array<int8_t,2> camera_axes(float x,float y,float dead_zone);
// Gyro rates: yaw positive turning left, pitch positive tilting up, rad/s. Result: mouse counts (x right, y down) for dt seconds, zero below the dead zone.
std::array<float,2> gyro_aim_counts(float yaw,float pitch,float dt,const GyroSettings&);
// Removes the resting offset of a gyro: the first half second after the first sample (restarted by any movement) is averaged as the offset, then it follows
// slow drift while the controller is nearly still. Returns the corrected (yaw, pitch); zero while measuring.
class GyroBias {
public:
    std::array<float,2> filter(float yaw,float pitch,uint64_t timestamp_ns);
private:
    bool calibrated_=false;
    uint64_t start_ns_=0;
    unsigned samples_=0;
    double sum_yaw_=0,sum_pitch_=0;
    float bias_yaw_=0,bias_pitch_=0;
};
}
