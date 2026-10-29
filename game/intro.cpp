#include "intro.h"
#include "dispatch.h"
#include "gx_vertices.h"
#include "wide_view.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
bool skip_intro; // assigned before the scheduler starts; guest baton owns subsequent access
constexpr uint32_t kStartupLogos=0x804D1E6C;
}
namespace uc { void intro_init(bool skip) { skip_intro=skip; } }

void hle_IntroLogoUpdate(ppc::Ctx& c) {
    const auto object=c.r[3];
    if (skip_intro && object==kStartupLogos && ppc::rd8(c,object+4) && ppc::rd8(c,object+5) && !ppc::rd8(c,object+0x28)) {
        const uint32_t count=ppc::rd32(c,object+0x38), rows=ppc::rd32(c,object+0x3C);
        gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
        if (!count) throw std::runtime_error("startup logo table is empty");
        memory.read(rows,size_t(count)*52);
        const uint64_t last=uint64_t(rows)+uint64_t(count-1)*52;
        if (last>UINT32_MAX-48) throw std::runtime_error("startup logo table address overflows");
        const float duration=ppc::rdf32(c,uint32_t(last)+48);
        if (!std::isfinite(duration) || duration<0) throw std::runtime_error("invalid startup logo duration");
        // Enter the translated update's normal final-row completion branch.
        ppc::wr32(c,object+0x34,count-1);
        ppc::wr32(c,object+0x24,4);
        ppc::wrf32(c,object+0x2C,duration);
        std::fprintf(stderr,"[intro] skipped Nintendo/CAPCOM/CRIWARE startup screens\n");
    }
    // Static lowering of8024585C's stack store;80245860 continues the original compiled body.
    const auto stack=c.r[1];
    ppc::wr32(c,stack-48,stack);
    c.r[1]=stack-48;
    uc::call_indirect(c,0x80245860);
}

// C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 near, f32 far): m[0][0] = cot(fovY / 2) / aspect. On a wider output the aspect grows by 1 / k
// (renderer/wide_view.h), so every projection the game builds, and everything it derives from it (the pointer's range, hit tests, culling), sees
// the wide picture. A taller output shrinks the field of view's tangent on the y axis instead. At 16:9 (or with the option off) the original runs unchanged.
void hle_MTXPerspective(ppc::Ctx& c) {
    // Static lowering of 80265464's stwu r1,-64(r1); 80265468 continues the original compiled body.
    const auto stack=c.r[1];
    ppc::wr32(c,stack-64,stack);
    c.r[1]=stack-64;
    {
        const auto factors=gx::view_factors();
        const float kx=factors.x,ky=factors.y;
        float aspect=float(c.f[2]),fov=float(c.f[1]);
        if((kx<1.f || ky<1.f) && std::isfinite(aspect) && aspect>0 && std::isfinite(fov) && fov>0 && fov<180) {   // at 16:9 the arguments are not touched at all
            if(kx<1.f) aspect/=kx;
            if(ky<1.f) {   // the vertical tangent grows by 1 / k_y; the horizontal one stays: cot' = cot * k_y, aspect' = aspect * k_y
                const double tangent=std::tan(double(fov)*3.14159265358979323846/360.0);
                fov=float(std::atan(tangent/ky)*360.0/3.14159265358979323846);
                aspect*=ky;
            }
            c.f[1]=fov; c.f[2]=aspect;
        }
    }
    uc::call_indirect(c,0x80265468);
}

void hle_MovieUpdate(ppc::Ctx& c) {
    const auto object=c.r[3];
    gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
    memory.read(object,0x50);
    if(ppc::rd16(c,object+6)==1) {
        memory.read(c.r[2]-25960,4);
        const auto minimum=ppc::rdf32(c,c.r[2]-25960);
        if(!std::isfinite(minimum) || minimum<0) throw std::runtime_error("invalid movie skip threshold");
        ppc::wrf32(c,object+8,minimum);
    }
    const auto update=[&] {
        // Static lowering of801426D4; the remaining update and cleanup stay AOT compiled.
        const auto stack=c.r[1];
        ppc::wr32(c,stack-32,stack); c.r[1]=stack-32;
        c.r[3]=object; uc::call_indirect(c,0x801426D8);
    };
    const auto finish_fade=[&] {
        ppc::wr32(c,object,uint32_t(-950));
        ppc::wr16(c,object+4,247); // Next +8 reaches255 and runs the original decoder cleanup.
    };
    if(ppc::rd16(c,object+6)==2) finish_fade();
    update();
    if(ppc::rd16(c,object+6)==2) { finish_fade(); update(); }
}
