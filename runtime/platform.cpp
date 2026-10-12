#include "clock.h"
#include "fatal.h"
#include "mmio.h"
#include "hle/os.h"
#include "ppc_rt.h"

namespace ppc {
uint32_t mmio_read(uint32_t ea, int size) { return uc::mmio_bus().read(ea, size); }
void mmio_write(uint32_t ea, int size, uint32_t v) { uc::mmio_bus().write(ea, size, v); }
[[noreturn]] void trap(uint32_t addr, const char* what) { uc::fatal_guest(addr, what); }
uint64_t timebase() { return uc::now_ticks(); }
void poll_slow(Ctx&) { uc::os_sched().safe_point(); }
}  // namespace ppc
