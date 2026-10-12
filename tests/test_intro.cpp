#include "check.h"
#include "intro.h"
#include "dispatch.h"
#include "ppc_fixture.h"
#include <exception>

static unsigned calls;
static bool accelerated;
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
int main() {
    Fix fixture; auto& c=fixture.c;
    const uc::FnEntry entries[]={{0x80245860,original_update}};
    uc::dispatch_init(entries,1,0x80245860,0x80245864);
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
}
