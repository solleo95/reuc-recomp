// The translator folds fcmp + cror + branch, fcmp + branch and fcmp + [cror] + mfcr + rlwinm. into direct comparisons of the operands
// (common/translator/emit.cpp). This runs the original instruction sequences against the expressions the translator emits, on ordinary
// values, zeros of both signs, infinities and NaNs.
#include "check.h"
#include "ppc_fixture.h"
#include <cmath>
#include <cstdio>
#include <limits>
using namespace ppc;

int main() {
    const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
    const double values[] = {0.0, -0.0, 1.0, -1.0, 2.5, -2.5, 1e-300, 1e300, inf, -inf, nan, -nan};
    int mismatches = 0, runs = 0;
    for (double x : values) for (double y : values) for (bool so : {false, true}) {
        Fix fix; Ctx& c = fix.c;
        c.f[1] = x; c.f[2] = y; c.xer_so = so;
        const double lhs = c.f[1], rhs = c.f[2];
        const auto bit_of = [&](Ctx& k, int bit) { return crbit(k, bit); };
        for (int variant = 0; variant < 5; ++variant) {
            Ctx k = c; k.r[0] = 0xDEADBEEF;
            bool expected = false, folded = false;
            uint32_t reg_expected = 0, reg_folded = 0;
            switch (variant) {
                case 0: op_fcmpo(k, 0, 1, 2); op_cror(k, 2, 0, 2); expected = bit_of(k, 2); folded = lhs <= rhs; break;   // le
                case 1: op_fcmpu(k, 0, 1, 2); op_cror(k, 2, 1, 2); expected = bit_of(k, 2); folded = lhs >= rhs; break;   // ge
                case 2: op_fcmpu(k, 0, 1, 2); expected = bit_of(k, 0); folded = lhs < rhs; break;
                case 3: op_fcmpu(k, 0, 1, 2); expected = bit_of(k, 1); folded = lhs > rhs; break;
                default: op_fcmpo(k, 0, 1, 2); expected = bit_of(k, 3); folded = (lhs != lhs || rhs != rhs); break;   // unordered
            }
            if (expected != folded) ++mismatches;
            ++runs;
        }
        // fcmp, cror, mfcr, rlwinm. r0,r0,3,31,31 against the bit test: the register and the record-form CR0 (as the branch reads it)
        {
            Ctx k = c; k.r[0] = 0xDEADBEEF;
            op_fcmpo(k, 0, 1, 2); op_cror(k, 2, 1, 2); op_mfcr(k, 0); op_rlwinm(k, 0, 0, 3, 31, 31, true);
            Ctx g = c; g.r[0] = 0xDEADBEEF;
            g.r[0] = uint32_t(g.f[1] >= g.f[2]);
            const bool original_taken = !crbit(k, 2), folded_taken = !(int32_t(g.r[0]) == 0);   // bne cr0
            if (k.r[0] != g.r[0] || original_taken != folded_taken) ++mismatches;
            cr0(g, g.r[0]);
            if (g.cr[0] != k.cr[0]) ++mismatches;
            ++runs;
        }
    }
    std::printf("fcmp fusion: %d mismatches in %d runs\n", mismatches, runs);
    CHECK(mismatches == 0);
    return 0;
}
