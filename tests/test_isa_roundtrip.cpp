#include "check.h"
#include "ppc/isa.h"
#include <cstdio>

// Every row must decode back to itself from a canonical word (primary opcode + xo, zero operands).
int main() {
    size_t n = 0;
    auto* const* ops = ppc::all_ops(n);
    int bad = 0;
    for (size_t i = 0; i < n; ++i) {
        const ppc::OpInfo& o = *ops[i];
        uint32_t w = uint32_t(o.primary) << 26 | (o.sub == ppc::Sub::N ? 0u : uint32_t(o.xo) << 1);
        const ppc::OpInfo* d = ppc::decode(w);
        if (d != &o) { std::fprintf(stderr, "row %s (primary %u xo %u) decodes to %s\n", o.name, o.primary, o.xo, d ? d->name : "null"); ++bad; }
    }
    CHECK(bad == 0);
    return 0;
}
