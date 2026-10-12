#pragma once
#include "ppc_rt.h"
namespace uc {
struct LowMemInfo {
    uint32_t mem1_size = 0x01800000, mem2_size = 0x04000000;
    uint32_t arena_lo = 0, arena_hi = 0x81700000;
    uint32_t mem2_arena_lo = 0x90000800, mem2_arena_hi = 0x933E0000;   // usable MEM2 (the top is left to IOS on a console)
    uint32_t ipc_buf_lo = 0x933E0000, ipc_buf_hi = 0x93400000;           // request buffers for the IOS call interface, just above the MEM2 arena
    uint32_t tv_format = 0;   // 0 NTSC, 1 PAL
};
uint32_t arena_lo_after_bss(uint32_t bss_addr, uint32_t bss_size);   // end of BSS rounded up to 32 bytes
void init_lowmem(ppc::Ctx& c, const uint8_t* disc_header32, const LowMemInfo& info);
}
