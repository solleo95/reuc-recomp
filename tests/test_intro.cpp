#include "check.h"
#include "intro.h"
#include "dispatch.h"
#include "ppc_fixture.h"
#include "wide_view.h"
#include <cmath>
#include <cstring>
#include <exception>

static unsigned calls;
static bool accelerated;
static bool movie_skip;
static unsigned movie_calls;
static void movie_update(ppc::Ctx& c) {
    ++movie_calls;
    const auto object=c.r[3];
    CHECK(ppc::rd32(c,c.r[1])==0x817D0000);
    if(ppc::rd16(c,object+6)==1) {
        CHECK(ppc::rdf32(c,object+8)>=2.f);
        if(movie_skip) ppc::wr16(c,object+6,2);
    } else if(ppc::rd16(c,object+6)==2) {
        CHECK(ppc::rd16(c,object+4)==247 && ppc::rd32(c,object)==uint32_t(-950));
        ppc::wr16(c,object+6,99);
    }
    c.r[1]=ppc::rd32(c,c.r[1]); c.r[3]=0;
}
static void original_update(ppc::Ctx& c) {
    ++calls;
    CHECK(ppc::rd32(c,c.r[1])==0x817D0000);
    CHECK(c.lr==0x80100000);
    if (accelerated) {
        CHECK(ppc::rd32(c,0x804D1E90)==4 && ppc::rd32(c,0x804D1EA0)==2);
        CHECK(ppc::rdf32(c,0x804D1E98)==12.f);
        ppc::wr8(c,0x804D1E94,1); // fixture's normal completion branch
    }
    c.r[1]=ppc::rd32(c,c.r[1]);
    c.r[3]=1;
}
static double seen_fov, seen_aspect;
static unsigned perspective_calls;
static void perspective_body(ppc::Ctx& c) {   // the original C_MTXPerspective body from 80265468: the wrapper has lowered the stwu
    ++perspective_calls;
    CHECK(ppc::rd32(c,c.r[1])==0x817D0000 && c.r[1]==0x817D0000-64);
    seen_fov=c.f[1]; seen_aspect=c.f[2];
    c.r[1]=ppc::rd32(c,c.r[1]);
}
int main() {
    Fix fixture; auto& c=fixture.c;
    const uc::FnEntry entries[]={{0x801426D8,movie_update},{0x80245860,original_update},{0x80265468,perspective_body}};
    uc::dispatch_init(entries,3,0x801426D8,0x80265470);
    c.r[1]=0x817D0000; c.lr=0x80100000;
    ppc::wr8(c,0x804D1E70,1); ppc::wr8(c,0x804D1E71,1);
    ppc::wr8(c,0x804D1E94,0); ppc::wr32(c,0x804D1EA4,3); ppc::wr32(c,0x804D1EA8,0x80002000);
    ppc::wrf32(c,0x80002000+2*52+48,12.f);
    uc::intro_init(false); c.r[3]=0x804D1E6C; hle_IntroLogoUpdate(c);
    CHECK(calls==1 && c.r[1]==0x817D0000 && ppc::rd8(c,0x804D1E94)==0);
    uc::intro_init(true); accelerated=true; c.r[3]=0x804D1E6C; hle_IntroLogoUpdate(c);
    CHECK(calls==2 && c.r[1]==0x817D0000 && c.r[3]==1 && ppc::rd8(c,0x804D1E94)==1);
    accelerated=false;
    // Other instances and not-yet-loaded startup objects retain normal behavior.
    c.r[3]=0x80003000; hle_IntroLogoUpdate(c);
    ppc::wr8(c,0x804D1E71,0); ppc::wr8(c,0x804D1E94,0);
    c.r[3]=0x804D1E6C; hle_IntroLogoUpdate(c); CHECK(calls==4);
    ppc::wr8(c,0x804D1E71,1); ppc::wr32(c,0x804D1EA8,0xCC008000);
    bool rejected=false;
    try { c.r[3]=0x804D1E6C; hle_IntroLogoUpdate(c); } catch (const std::exception&) { rejected=true; }
    CHECK(rejected && calls==4 && c.r[1]==0x817D0000);
    c.r[2]=0x80010000;
    ppc::wrf32(c,c.r[2]-25960,2.f);
    ppc::wr16(c,0x80003006,1); ppc::wrf32(c,0x80003008,0.f);
    c.r[3]=0x80003000; hle_MovieUpdate(c);
    CHECK(movie_calls==1 && ppc::rd16(c,0x80003006)==1);
    movie_skip=true; c.r[3]=0x80003000; hle_MovieUpdate(c);
    CHECK(movie_calls==3 && ppc::rd16(c,0x80003006)==99 && c.r[1]==0x817D0000);
    c.r[3]=0x80003000; hle_MovieUpdate(c);
    CHECK(movie_calls==4 && ppc::rd16(c,0x80003006)==99);
    {   // C_MTXPerspective: untouched at 16:9, a wider aspect on a wider output, a larger vertical tangent on a taller one, nothing on odd arguments
        const double odd_fov=1.0/3, odd_aspect=1.0/7;   // not representable as floats: the original must see exactly these at 16:9
        c.r[1]=0x817D0000; c.lr=0x80100000; c.f[1]=odd_fov; c.f[2]=odd_aspect;
        gx::set_view_factors({});
        hle_MTXPerspective(c);
        CHECK(perspective_calls==1 && seen_fov==odd_fov && seen_aspect==odd_aspect && c.r[1]==0x817D0000);
        gx::set_view_factors(gx::factors_for_aspect(21.0/9));
        c.f[1]=60.0; c.f[2]=1.6;
        hle_MTXPerspective(c);
        CHECK(perspective_calls==2 && seen_fov==60.0 && std::abs(seen_aspect-1.6f/(16.0f/21.0f))<1e-5);
        gx::set_view_factors(gx::factors_for_aspect(16.0/10));
        c.f[1]=60.0; c.f[2]=1.6;
        hle_MTXPerspective(c);
        const double ky=(16.0/10)/(16.0/9);
        CHECK(perspective_calls==3 && std::abs(1/std::tan(seen_fov*3.14159265358979323846/360.0)-1/std::tan(60.0*3.14159265358979323846/360.0)*ky)<1e-4);   // cot' = cot * ky
        CHECK(std::abs(seen_aspect-1.6f*ky)<1e-4 && seen_fov>60.0 && seen_fov<180.0);
        gx::set_view_factors(gx::factors_for_aspect(21.0/9));
        for(const double bad:{0.0,-1.0,std::nan(""),180.0,400.0}) {   // an argument that is no field of view is left to the original
            c.f[1]=bad; c.f[2]=1.6;
            hle_MTXPerspective(c);
            CHECK(std::isnan(bad) ? std::isnan(seen_fov) : seen_fov==bad);
            CHECK(seen_aspect==1.6);
        }
        gx::set_view_factors({});
    }
}
