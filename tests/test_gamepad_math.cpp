#include "check.h"
#include "gamepad_math.h"
#include <cmath>
using namespace uc;
static bool near(float a,float b,float tolerance=1e-4f) { return std::fabs(a-b)<=tolerance; }
static void stick_aim() {
    const StickCurve curve;   // dead zone .15, speed 1.2 screen heights per second
    auto v=stick_aim_velocity(.1f,0,curve); CHECK(v[0]==0 && v[1]==0);                     // inside the dead zone
    v=stick_aim_velocity(0,-.14f,curve); CHECK(v[0]==0 && v[1]==0);
    v=stick_aim_velocity(1,0,curve); CHECK(near(v[0],1.2f) && v[1]==0);                     // full deflection is the full speed
    v=stick_aim_velocity(.575f,0,curve); CHECK(near(v[0],.3f));                             // ((.575-.15)/.85)^2 * 1.2: the curve is quadratic after the rescale
    v=stick_aim_velocity(0,-1,curve); CHECK(v[0]==0 && near(v[1],-1.2f));                   // the sign follows the stick (screen y points down)
    v=stick_aim_velocity(1,1,curve); CHECK(near(std::hypot(v[0],v[1]),1.2f,1e-3f) && near(v[0],v[1]));   // a corner is not faster than an edge
    v=stick_aim_velocity(.5f,0,{.15f,2.4f}); CHECK(near(v[0],2.4f*std::pow((.5f-.15f)/.85f,2.f)));
}
static void camera() {
    auto c=camera_axes(1,0,.1f); CHECK(c[0]==127 && c[1]==0);
    c=camera_axes(-1,0,.1f); CHECK(c[0]==-127 && c[1]==0);
    c=camera_axes(.05f,.05f,.1f); CHECK(c[0]==0 && c[1]==0);                                // radial dead zone
    c=camera_axes(.55f,0,.1f); CHECK(c[0]==64 || c[0]==63);                                 // (.55-.1)/.9 = .5 of full
    c=camera_axes(-1,1,.1f); CHECK(std::abs(c[0]+90)<=1 && std::abs(c[1]-90)<=1);           // a corner is clamped to the unit circle
    c=camera_axes(.3f,0,.1f); const auto d=camera_axes(.6f,0,.1f); CHECK(c[0]>0 && d[0]>c[0]);   // analog: more deflection, more camera
}
static void gyro_aim() {
    GyroSettings gyro; gyro.mode=GyroMode::always;
    auto c=gyro_aim_counts(1,0,.01f,gyro); CHECK(near(c[0],-kGyroCountsPerRadian*.01f,1e-3f) && c[1]==0);   // turning left moves the pointer left
    c=gyro_aim_counts(0,1,.01f,gyro); CHECK(c[0]==0 && near(c[1],-kGyroCountsPerRadian*.01f,1e-3f));       // tilting up moves the pointer up
    c=gyro_aim_counts(.005f,.005f,.01f,gyro); CHECK(c[0]==0 && c[1]==0);                                    // below the .0087 rad/s dead zone: a controller on a desk
    gyro.invert_x=true; c=gyro_aim_counts(1,0,.01f,gyro); CHECK(c[0]>0);
    gyro.invert_x=false; gyro.sensitivity_x=2; gyro.sensitivity_y=.5f;
    c=gyro_aim_counts(1,1,.01f,gyro); CHECK(near(c[0],-2*kGyroCountsPerRadian*.01f,1e-3f) && near(c[1],-.5f*kGyroCountsPerRadian*.01f,1e-3f));
}
static void gyro_bias() {
    GyroBias bias; uint64_t t=1000000000ull; const float offset=.02f;
    for(int i=0;i<150;++i) {auto r=bias.filter(offset,-offset,t); CHECK(r[0]==0 && r[1]==0); t+=4000000ull;}   // the first half second only measures
    for(int i=0;i<50;++i) {auto r=bias.filter(offset,-offset,t); CHECK(std::fabs(r[0])<1e-3f && std::fabs(r[1])<1e-3f); t+=4000000ull;}   // a constant offset is cancelled
    auto moved=bias.filter(offset+.5f,-offset,t); CHECK(near(moved[0],.5f,1e-3f));                           // real motion passes through
    GyroBias late; t=1000000000ull;                                                                          // picked up while connecting: the measurement restarts and ends on the resting value
    for(int i=0;i<60;++i) {late.filter(.8f,.8f,t); t+=4000000ull;}
    for(int i=0;i<160;++i) {late.filter(.01f,.01f,t); t+=4000000ull;}
    auto rest=late.filter(.01f,.01f,t); CHECK(std::fabs(rest[0])<1e-3f);
}
int main() { stick_aim(); camera(); gyro_aim(); gyro_bias(); }
