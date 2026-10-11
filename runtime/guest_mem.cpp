#include "guest_mem.h"
#include <cstdio>
#include <cstdlib>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace uc {
static uint8_t* g_base;

void guest_mem_init() {
    if (g_base) return;
    g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x40000000, MEM_RESERVE, PAGE_NOACCESS));
    if (!g_base || !VirtualAlloc(g_base, 0x01800000, MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc(g_base + 0x10000000, 0x04000000, MEM_COMMIT, PAGE_READWRITE) ||
        // locked cache: guest 0xE0000000..0xE0003FFF masks to offset 0x20000000 (used by dcbz_l, psq and lfs in the SDK)
        !VirtualAlloc(g_base + 0x20000000, 0x4000, MEM_COMMIT, PAGE_READWRITE)) {
        std::fprintf(stderr, "fatal: cannot reserve guest memory\n");
        std::exit(4);
    }
}
uint8_t* guest_mem_base() { return g_base; }
}  // namespace uc
