#include "dispatch.h"
#include <vector>

namespace uc {
static std::vector<Fn> g_dense;
static uint32_t g_begin, g_bytes;

void dispatch_init(const FnEntry* table, size_t count, uint32_t text_begin, uint32_t text_end) {
    g_begin = text_begin;
    g_bytes = text_end - text_begin;
    g_dense.assign(g_bytes / 4, nullptr);
    for (size_t i = 0; i < count; ++i) g_dense[(table[i].addr - text_begin) / 4] = table[i].fn;
}
bool has_function(uint32_t a) { return !(a & 3) && a - g_begin < g_bytes && g_dense[(a - g_begin) >> 2] != nullptr; }
void call_indirect(ppc::Ctx& c, uint32_t a) {
    if (has_function(a)) { g_dense[(a - g_begin) >> 2](c); return; }
    ppc::trap(a, "indirect call or jump to an address that is not a known function entry (add `entry <hex>` to project/uc.cfg)");
}
void hle_sc(ppc::Ctx&, uint32_t pc) { ppc::trap(pc, "sc (system call) is not implemented"); }
}  // namespace uc
