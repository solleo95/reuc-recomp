#include "ppc_rt.h"
#include <cstdio>
#include <cstdlib>

uint32_t g_mmio_last_addr, g_mmio_last_val;
int g_mmio_last_size;

namespace ppc {
uint32_t mmio_read(uint32_t ea, int size) { g_mmio_last_addr = ea; g_mmio_last_size = size; return 0xA5A5A5A5u; }
void mmio_write(uint32_t ea, int size, uint32_t v) { g_mmio_last_addr = ea; g_mmio_last_size = size; g_mmio_last_val = v; }
[[noreturn]] void trap(uint32_t addr, const char* what) {
    std::fprintf(stderr, "guest trap at %08X: %s\n", addr, what);
    std::exit(3);
}
uint64_t timebase() { return 0x1122334455667788ull; }
}  // namespace ppc
