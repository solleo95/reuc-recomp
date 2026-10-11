#include "check.h"
#include "ppc_fixture.h"
#include <cmath>
using namespace ppc;

static uint64_t bits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

int main() {
    Fix f; Ctx& c = f.c;

    // single vs double rounding
    c.f[1] = 1.0; c.f[2] = 1e-10;
    op_fadd(c, 3, 1, 2, false);   CHECK(c.f[3] == 1.0 + 1e-10);
    op_fadds(c, 3, 1, 2, false);  CHECK(c.f[3] == 1.0);
    // fused multiply-add keeps the exact product: (1+2^-30)*(1-2^-30) - 1 = -2^-60 exactly under fusion
    c.f[1] = 1.0 + std::ldexp(1.0, -30); c.f[2] = 1.0 - std::ldexp(1.0, -30); c.f[3] = -1.0;
    op_fmadd(c, 4, 1, 3, 2, false);   // frD = frA*frC + frB   (args: t, a, b, c)
    CHECK(c.f[4] == -std::ldexp(1.0, -60));
    op_fmsub(c, 4, 1, 3, 2, false);   // frA*frC - frB  with frB = -1
    CHECK(c.f[4] == std::fma(c.f[1], c.f[2], 1.0));
    op_fnmadd(c, 4, 1, 3, 2, false);  CHECK(c.f[4] == -(std::fma(c.f[1], c.f[2], -1.0)));
    // fmul takes (t, a, c): frD = frA * frC
    c.f[1] = 3.0; c.f[2] = 4.0;       op_fmul(c, 5, 1, 2, false); CHECK(c.f[5] == 12.0);
    op_fdiv(c, 5, 2, 1, false);       CHECK(c.f[5] == 4.0 / 3.0);
    // fsel: frD = (frA >= 0) ? frC : frB   (args: t, a, b, c)
    c.f[1] = -1.0; c.f[2] = 10.0; c.f[3] = 20.0;
    op_fsel(c, 4, 1, 2, 3, false);    CHECK(c.f[4] == 10.0);   // a<0 -> frB
    c.f[1] = 0.0;  op_fsel(c, 4, 1, 2, 3, false); CHECK(c.f[4] == 20.0);   // a>=0 -> frC
    // fctiw: round to nearest even; fctiwz truncates; both clamp; result lives in the low word
    c.f[1] = 2.5;  op_fctiw(c, 2, 1, false);  CHECK(uint32_t(bits(c.f[2])) == 2);
    c.f[1] = 3.5;  op_fctiw(c, 2, 1, false);  CHECK(uint32_t(bits(c.f[2])) == 4);
    c.f[1] = 3.9;  op_fctiwz(c, 2, 1, false); CHECK(uint32_t(bits(c.f[2])) == 3);
    c.f[1] = -3.9; op_fctiwz(c, 2, 1, false); CHECK(uint32_t(bits(c.f[2])) == 0xFFFFFFFD);
    c.f[1] = 1e20; op_fctiwz(c, 2, 1, false); CHECK(uint32_t(bits(c.f[2])) == 0x7FFFFFFF);
    c.f[1] = -1e20; op_fctiwz(c, 2, 1, false); CHECK(uint32_t(bits(c.f[2])) == 0x80000000);
    c.r[3] = 0x80003000; c.r[4] = 0;
    op_stfiwx(c, 2, 3, 4);            CHECK(rd32(c, 0x80003000) == 0x80000000);
    // frsp, fmr, fneg, fabs, fnabs
    c.f[1] = 1.0000000001; op_frsp(c, 2, 1, false); CHECK(c.f[2] == 1.0);
    c.f[1] = -2.0; op_fabs(c, 2, 1, false);  CHECK(c.f[2] == 2.0);
    op_fneg(c, 2, 1, false);  CHECK(c.f[2] == 2.0);
    op_fnabs(c, 2, 1, false); CHECK(c.f[2] == -2.0);
    op_fmr(c, 2, 1, false);   CHECK(c.f[2] == -2.0);
    // fres / frsqrte are estimates; require 1/4096 relative accuracy
    c.f[1] = 8.0;  op_fres(c, 2, 1, false);    CHECK(std::fabs(c.f[2] - 0.125) < 0.125 / 4096);
    c.f[1] = 16.0; op_frsqrte(c, 2, 1, false); CHECK(std::fabs(c.f[2] - 0.25) < 0.25 / 4096);
    // fcmpu / fcmpo: LT=8 GT=4 EQ=2 unordered=1
    c.f[1] = 1.0; c.f[2] = 2.0;
    op_fcmpu(c, 1, 1, 2); CHECK(c.cr[1] == 8);
    op_fcmpu(c, 1, 2, 1); CHECK(c.cr[1] == 4);
    op_fcmpu(c, 1, 1, 1); CHECK(c.cr[1] == 2);
    c.f[3] = std::nan("");
    op_fcmpu(c, 1, 1, 3); CHECK(c.cr[1] == 1);
    // loads/stores: lfs converts, lfd/stfd round-trip, lfs also fills ps1
    c.r[3] = 0x80003100; wrf32(c, 0x80003100, 1.5f);
    op_lfs(c, 5, 3, 0);   CHECK(c.f[5] == 1.5 && c.ps1[5] == 1.5);
    c.f[6] = 6.125;       op_stfd(c, 6, 3, 8); CHECK(rdf64(c, 0x80003108) == 6.125);
    op_lfd(c, 7, 3, 8);   CHECK(c.f[7] == 6.125);
    op_stfs(c, 6, 3, 16); CHECK(rdf32(c, 0x80003110) == 6.125f);
    // FPSCR access
    c.fpscr = 0x1234; op_mffs(c, 8, false); CHECK(uint32_t(bits(c.f[8])) == 0x1234);

    // scalar single-precision results are written to both paired-single halves (Gekko), double-precision ones only to ps0
    c.f[1] = 1.5; c.f[2] = 2.5; c.ps1[3] = 99.0; c.ps1[4] = 99.0;
    op_fadds(c, 3, 1, 2, false);   CHECK(c.f[3] == 4.0 && c.ps1[3] == 4.0);
    op_fsubs(c, 4, 2, 1, false);   CHECK(c.f[4] == 1.0 && c.ps1[4] == 1.0);
    c.ps1[5] = 99.0; op_fmuls(c, 5, 1, 2, false);   CHECK(c.f[5] == 3.75 && c.ps1[5] == 3.75);
    c.ps1[5] = 99.0; op_fdivs(c, 5, 2, 1, false);   CHECK(c.ps1[5] == c.f[5]);
    c.ps1[5] = 99.0; op_frsp(c, 5, 1, false);       CHECK(c.f[5] == 1.5 && c.ps1[5] == 1.5);
    c.ps1[5] = 99.0; op_fmadds(c, 5, 1, 2, 1, false); CHECK(c.ps1[5] == c.f[5]);
    c.ps1[5] = 99.0; op_fres(c, 5, 2, false);       CHECK(c.ps1[5] == c.f[5]);
    c.ps1[6] = 99.0; op_fadd(c, 6, 1, 2, false);    CHECK(c.f[6] == 4.0 && c.ps1[6] == 99.0);
    // ---- paired singles ----
    c.f[1] = 1.0; c.ps1[1] = 2.0; c.f[2] = 10.0; c.ps1[2] = 20.0;
    op_ps_add(c, 3, 1, 2, false);      CHECK(c.f[3] == 11.0 && c.ps1[3] == 22.0);
    op_ps_sub(c, 3, 2, 1, false);      CHECK(c.f[3] == 9.0 && c.ps1[3] == 18.0);
    op_ps_mul(c, 3, 1, 2, false);      CHECK(c.f[3] == 10.0 && c.ps1[3] == 40.0);   // ps_mul(t, a, c)
    op_ps_merge00(c, 3, 1, 2, false);  CHECK(c.f[3] == 1.0 && c.ps1[3] == 10.0);
    op_ps_merge01(c, 3, 1, 2, false);  CHECK(c.f[3] == 1.0 && c.ps1[3] == 20.0);
    op_ps_merge10(c, 3, 1, 2, false);  CHECK(c.f[3] == 2.0 && c.ps1[3] == 10.0);
    op_ps_merge11(c, 3, 1, 2, false);  CHECK(c.f[3] == 2.0 && c.ps1[3] == 20.0);
    op_ps_muls0(c, 3, 1, 2, false);    CHECK(c.f[3] == 10.0 && c.ps1[3] == 20.0);   // ps_muls0(t, a, c): both scaled by c.ps0
    op_ps_muls1(c, 3, 1, 2, false);    CHECK(c.f[3] == 20.0 && c.ps1[3] == 40.0);
    op_ps_madd(c, 3, 1, 1, 2, false);  CHECK(c.f[3] == 1.0 * 10.0 + 1.0 && c.ps1[3] == 2.0 * 20.0 + 2.0);  // t = a*c + b
    op_ps_sum0(c, 3, 1, 2, 1, false);  CHECK(c.f[3] == 1.0 + 20.0 && c.ps1[3] == 2.0);  // ps0 = a.ps0 + b.ps1 ; ps1 = c.ps1
    op_ps_neg(c, 3, 1, false);         CHECK(c.f[3] == -1.0 && c.ps1[3] == -2.0);
    op_ps_abs(c, 3, 3, false);         CHECK(c.f[3] == 1.0 && c.ps1[3] == 2.0);
    op_ps_mr(c, 4, 2, false);          CHECK(c.f[4] == 10.0 && c.ps1[4] == 20.0);

    // quantized load: GQR0 = type s16 (7), scale 2 -> value * 2^-2
    wr16(c, 0x80003200, 0x0040); wr16(c, 0x80003202, 0xFFC0);
    c.gqr[0] = (2u << 24) | (7u << 16);
    c.r[3] = 0x80003200;
    op_psq_l(c, 5, 3, 0, 0, 0);        CHECK(c.f[5] == 16.0 && c.ps1[5] == -16.0);
    op_psq_l(c, 5, 3, 0, 1, 0);        CHECK(c.f[5] == 16.0 && c.ps1[5] == 1.0);    // W=1: one element, ps1 = 1.0
    // quantized store: u8 (4), scale 0 clamps to 0..255 and truncates
    c.gqr[1] = 4u;                       // ST_TYPE=4, ST_SCALE=0
    c.f[6] = 300.0; c.ps1[6] = -5.0;
    c.r[3] = 0x80003300;
    op_psq_st(c, 6, 3, 0, 0, 1);       CHECK(rd8(c, 0x80003300) == 255 && rd8(c, 0x80003301) == 0);
    c.f[6] = 7.9;  op_psq_st(c, 6, 3, 0, 1, 1); CHECK(rd8(c, 0x80003300) == 7);   // W=1 stores one element
    // float type passes through: GQR2 type 0
    c.gqr[2] = 0; wrf32(c, 0x80003400, 1.25f); wrf32(c, 0x80003404, -2.5f); c.r[3] = 0x80003400;
    op_psq_l(c, 7, 3, 0, 0, 2);        CHECK(c.f[7] == 1.25 && c.ps1[7] == -2.5);
    // ps_cmpu0 / ps_cmpu1 write a CR field like fcmp
    op_ps_cmpu0(c, 2, 1, 2);           CHECK(c.cr[2] == 8);
    op_ps_cmpu1(c, 2, 1, 2);           CHECK(c.cr[2] == 8);
    return 0;
}
