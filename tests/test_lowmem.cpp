#include "check.h"
#include "lowmem.h"
#include "ppc_fixture.h"
using namespace ppc;

int main() {
    Fix f; Ctx& c = f.c;
    uint8_t hdr[32] = {'R', 'B', 'U', 'E', '0', '8', 0, 0};
    uc::LowMemInfo info;
    info.arena_lo = uc::arena_lo_after_bss(0x8043A680, 0x002910FC);
    CHECK(info.arena_lo == 0x806CB780);            // end of BSS 0x806CB77C rounded up to 32 bytes
    uc::init_lowmem(c, hdr, info);
    CHECK(rd32(c, 0x80000000) == 0x52425545);       // "RBUE" copied from the disc header
    CHECK(rd16(c, 0x80000004) == 0x3038);           // "08"
    CHECK(rd32(c, 0x80000020) == 0x0D15EA5E);
    CHECK(rd32(c, 0x80000028) == 0x01800000);       // MEM1 size
    CHECK(rd32(c, 0x80000030) == info.arena_lo);
    CHECK(rd32(c, 0x80000034) == 0x81700000);
    CHECK(rd32(c, 0x800000F8) == 243000000);        // bus clock
    CHECK(rd32(c, 0x800000FC) == 729000000);        // CPU clock
    CHECK(rd32(c, 0x800000CC) == 0);                // NTSC
    CHECK(rd32(c, 0x80003100) == 0x01800000);       // physical MEM1 size
    CHECK(rd32(c, 0x80003118) == 0x04000000);       // MEM2 physical size
    CHECK(rd32(c, 0x80003124) == 0x90000800);       // MEM2 arena low (OSInit installs it as the arena start)
    CHECK(rd32(c, 0x80003128) == 0x933E0000);       // MEM2 arena high
    CHECK(rd32(c, 0x80003130) == 0x933E0000);       // IPC buffer low (__OSInitIPCBuffer copies it)
    CHECK(rd32(c, 0x80003134) == 0x93400000);       // IPC buffer high
    CHECK(rd32(c, 0x80003140) == 0);                // everything else stays zero (R0-layouts.md)
    return 0;
}
