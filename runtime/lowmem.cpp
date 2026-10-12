#include "lowmem.h"

namespace uc {

uint32_t arena_lo_after_bss(uint32_t bss_addr, uint32_t bss_size) { return (bss_addr + bss_size + 31u) & ~31u; }

// Only words with a proven value are written; see docs/spikes/R0-layouts.md. Everything else starts as zero.
void init_lowmem(ppc::Ctx& c, const uint8_t* hdr, const LowMemInfo& i) {
    for (uint32_t k = 0; k < 32; ++k) ppc::wr8(c, 0x80000000u + k, hdr[k]);   // disc header: game id, maker, version
    ppc::wr32(c, 0x80000020, 0x0D15EA5E);      // boot magic
    ppc::wr32(c, 0x80000024, 1);               // version
    ppc::wr32(c, 0x80000028, i.mem1_size);
    ppc::wr32(c, 0x8000002C, 0x23);             // production-board metadata consumed by OSGetConsoleType
    ppc::wr32(c, 0x80000030, i.arena_lo);
    ppc::wr32(c, 0x80000034, i.arena_hi);
    ppc::wr32(c, 0x800000CC, i.tv_format);
    ppc::wr32(c, 0x800000F8, 243000000);       // bus clock
    ppc::wr32(c, 0x800000FC, 729000000);       // CPU clock
    ppc::wr32(c, 0x80003100, i.mem1_size);     // physical MEM1 size (OSGetPhysicalMem1Size)
    ppc::wr32(c, 0x80003104, i.mem1_size);     // simulated MEM1 size
    ppc::wr32(c, 0x80003118, i.mem2_size);     // physical MEM2 size (OSGetPhysicalMem2Size)
    ppc::wr32(c, 0x8000311C, i.mem2_size);     // simulated MEM2 size
    ppc::wr32(c, 0x80003124, i.mem2_arena_lo); // MEM2 arena bounds: OSInit passes them to OSSetMEM2ArenaLo/Hi when non-zero
    ppc::wr32(c, 0x80003128, i.mem2_arena_hi);
    ppc::wr32(c, 0x80003130, i.ipc_buf_lo);    // __OSInitIPCBuffer
    ppc::wr32(c, 0x80003134, i.ipc_buf_hi);
}

}  // namespace uc
