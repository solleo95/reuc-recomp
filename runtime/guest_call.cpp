#include "guest_call.h"
#include "dispatch.h"
namespace uc {
uint32_t call_guest(ppc::Ctx& c, uint32_t addr, std::initializer_list<uint32_t> args) {
    int i = 3;
    for (uint32_t a : args) c.r[i++] = a;
    uint32_t saved_lr = c.lr;
    call_indirect(c, addr);
    c.lr = saved_lr;
    return c.r[3];
}
}  // namespace uc
