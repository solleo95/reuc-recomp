#include "check.h"
#include "ppc_fixture.h"
#include <cmath>
#include <initializer_list>
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
    // float pair store: big-endian bytes, rounded to single, and the write barrier stamps both pages of a store that crosses a 4 KiB boundary
    c.f[8] = 1.5; c.ps1[8] = -0.1; c.r[3] = 0x80004FFC;
    gx::page_epoch[0x4000 >> 12] = gx::page_epoch[0x5000 >> 12] = 0; gx::write_epoch = 7;
    op_psq_st(c, 8, 3, 0, 0, 2);
    CHECK(rd32(c, 0x80004FFC) == 0x3FC00000u && rd32(c, 0x80005000) == 0xBDCCCCCDu);
    CHECK(gx::page_epoch[0x4000 >> 12] == 7 && gx::page_epoch[0x5000 >> 12] == 7);
    op_psq_l(c, 9, 3, 0, 0, 2);        CHECK(c.f[9] == 1.5 && c.ps1[9] == double(-0.1f));
    gx::page_epoch[0x6000 >> 12] = 0; c.r[3] = 0x80006000; op_psq_st(c, 8, 3, 0, 0, 2);
    CHECK(gx::page_epoch[0x6000 >> 12] == 7 && gx::page_epoch[0x7000 >> 12] != 7);   // inside one page: only that page
    // single float (W=1): a load leaves ps1 = 1.0, a store writes only the four bytes
    c.gqr[2] = 0; wrf32(c, 0x80003500, 2.5f); wr32(c, 0x80003504, 0xDEADBEEFu); c.r[3] = 0x80003500;
    op_psq_l(c, 7, 3, 0, 1, 2);        CHECK(c.f[7] == 2.5 && c.ps1[7] == 1.0);
    c.f[7] = -3.75; op_psq_st(c, 7, 3, 0, 1, 2);   CHECK(rdf32(c, 0x80003500) == -3.75f && rd32(c, 0x80003504) == 0xDEADBEEFu);
    // quantized elements use the exact powers of two of the table: same result as ldexp for every scale and integer type
    for (int type = 4; type <= 7; ++type) for (int scale = -32; scale < 32; ++scale) {
        c.gqr[3] = (uint32_t(scale) & 0x3Fu) << 24 | uint32_t(type) << 16 | (uint32_t(scale) & 0x3Fu) << 8 | uint32_t(type);
        for (double v : {0.0, 1.0, -1.0, 12.5, 300.25, -70000.5, 1e-3, 1e9}) {
            c.f[6] = v; c.ps1[6] = -v; c.r[3] = 0x80003600;
            op_psq_st(c, 6, 3, 0, 0, 3);
            const bool one = type == 4 || type == 6;
            const double lo = type == 4 || type == 5 ? 0 : type == 6 ? -128 : -32768, hi = type == 4 ? 255 : type == 5 ? 65535 : type == 6 ? 127 : 32767;
            auto quantize = [&](double x) { double q = std::trunc(x * std::ldexp(1.0, scale)); return std::isnan(q) ? lo : q < lo ? lo : q > hi ? hi : q; };
            const double q0 = quantize(v), q1 = quantize(-v);
            const int32_t raw0 = one ? int32_t(rd8(c, 0x80003600)) : int32_t(rd16(c, 0x80003600)), raw1 = one ? int32_t(rd8(c, 0x80003601)) : int32_t(rd16(c, 0x80003602));
            CHECK(raw0 == (one ? int32_t(uint8_t(int32_t(q0))) : int32_t(uint16_t(int32_t(q0)))));
            CHECK(raw1 == (one ? int32_t(uint8_t(int32_t(q1))) : int32_t(uint16_t(int32_t(q1)))));
            op_psq_l(c, 5, 3, 0, 0, 3);
            const double l0 = (type == 4 ? double(uint8_t(raw0)) : type == 5 ? double(uint16_t(raw0)) : type == 6 ? double(int8_t(raw0)) : double(int16_t(raw0))) * std::ldexp(1.0, -scale);
            CHECK(c.f[5] == l0);
        }
    }
    // r1 (stack) accesses skip the MMIO test and the write barrier, r2/r13 skip only the MMIO test, other bases keep both; values agree on every path
    for (int base : {1, 2, 13, 3}) {
        const uint32_t page = 0x80010000u + uint32_t(base) * 0x1000u;
        gx::page_epoch[(page & 0x3FFFFFFFu) >> 12] = 0; gx::write_epoch = 9;
        c.r[base] = page; c.r[5] = 0x11223344u; c.f[20] = 6.25;
        op_stw(c, 5, base, 4);   CHECK(rd32(c, page + 4) == 0x11223344u);
        op_stb(c, 5, base, 9);   CHECK(rd8(c, page + 9) == 0x44u);
        op_sth(c, 5, base, 10);  CHECK(rd16(c, page + 10) == 0x3344u);
        op_stfd(c, 20, base, 16); CHECK(rdf64(c, page + 16) == 6.25);
        op_stfs(c, 20, base, 24); CHECK(rdf32(c, page + 24) == 6.25f);
        op_lwz(c, 6, base, 4);   CHECK(c.r[6] == 0x11223344u);
        op_lhz(c, 6, base, 10);  CHECK(c.r[6] == 0x3344u);
        op_lbz(c, 6, base, 9);   CHECK(c.r[6] == 0x44u);
        c.f[21] = 0; op_lfd(c, 21, base, 16); CHECK(c.f[21] == 6.25);
        c.f[21] = 0; op_lfs(c, 21, base, 24); CHECK(c.f[21] == 6.25 && c.ps1[21] == 6.25);
        CHECK(gx::page_epoch[(page & 0x3FFFFFFFu) >> 12] == (base == 1 ? 0u : 9u));
    }
    // Paired float stack traffic uses the same unwatched RAM path as scalar stack stores.
    for(int base : {1,2,13,3}) {
        const uint32_t address=0x80015FFCu;
        c.r[base]=address; c.gqr[0]=0; c.f[20]=1.5; c.ps1[20]=-0.1;
        gx::page_epoch[0x15000>>12]=gx::page_epoch[0x16000>>12]=0;
        op_psq_st(c,20,base,0,0,0);
        CHECK(rd32(c,address)==0x3FC00000u && rd32(c,address+4)==0xBDCCCCCDu);
        CHECK(gx::page_epoch[0x15000>>12]==(base==1 ? 0u : 9u));
        CHECK(gx::page_epoch[0x16000>>12]==(base==1 ? 0u : 9u));
        op_psq_l(c,21,base,0,0,0);
        CHECK(c.f[21]==1.5 && c.ps1[21]==double(-0.1f));
        gx::page_epoch[0x15000>>12]=0;
        op_psq_st(c,20,base,-4,1,0);
        CHECK(gx::page_epoch[0x15000>>12]==(base==1 ? 0u : 9u));
        op_psq_l(c,21,base,-4,1,0);
        CHECK(c.f[21]==1.5 && c.ps1[21]==1.0);
    }
    // Float pairs preserve scalar conversion bits, including signed zero, NaNs and subnormals.
    for(uint32_t bits : {0u,0x80000000u,1u,0x80000001u,0x007FFFFFu,0x7F7FFFFFu,0x7F800000u,0xFF800000u,0x7FC12345u,0x7F812345u}) {
        float value;std::memcpy(&value,&bits,4);
        volatile float scalar_input=value; // Force scalar conversions instead of folding a signaling-NaN round trip.
        const double expected=double(scalar_input);
        c.gqr[0]=0;c.r[3]=0x80015FFCu;
        wr32(c,c.r[3],bits);wr32(c,c.r[3]+4,bits^0x80000000u);
        op_psq_l(c,20,3,0,0,0);
        CHECK(std::memcmp(&c.f[20],&expected,8)==0);
        const double opposite=-expected;
        CHECK(std::memcmp(&c.ps1[20],&opposite,8)==0);
        volatile double scalar_output=expected;
        const float rounded=float(scalar_output);uint32_t rounded_bits;std::memcpy(&rounded_bits,&rounded,4);
        op_psq_st(c,20,3,0,0,0);
        if(rd32(c,c.r[3])!=rounded_bits || rd32(c,c.r[3]+4)!=(rounded_bits^0x80000000u))
            std::fprintf(stderr,"PSQ bits %08x expected %08x actual %08x:%08x\n",bits,rounded_bits,rd32(c,c.r[3]),rd32(c,c.r[3]+4));
        CHECK(rd32(c,c.r[3])==rounded_bits && rd32(c,c.r[3]+4)==(rounded_bits^0x80000000u));
    }
    for(double value : {1.0000000596046448,1.0000001788139343,-1.0000000596046448,1e-46,-1e-46,1e39,-1e39}) {
        volatile double scalar=value;
        const float rounded=float(scalar);uint32_t expected;std::memcpy(&expected,&rounded,4);
        c.f[20]=value;c.ps1[20]=-value;
        op_psq_st(c,20,3,0,0,0);
        CHECK(rd32(c,c.r[3])==expected && rd32(c,c.r[3]+4)==(expected^0x80000000u));
    }
    // Pair arithmetic keeps double intermediates, single rounding and aliased source registers.
    using PairBinary = void (*)(Ctx&,int,int,int,bool);
    const PairBinary pair_binary[]={op_ps_add,op_ps_sub,op_ps_mul,op_ps_muls0,op_ps_muls1};
    const double pair_values[]={0.0,-0.0,1.0000000596046448,-1.0000001788139343,1e-320,1e-46,1e-40,1e39,-1e39,INFINITY,-INFINITY,NAN};
    for(size_t operation=0;operation<5;++operation) for(int target:{0,1,2}) for(bool record:{false,true})
        for(double a:pair_values) for(double b:pair_values) {
            c.f[1]=a;c.ps1[1]=-b;c.f[2]=b;c.ps1[2]=a;
            const auto scalar=[&](double x,double y) {
                volatile double left=x,right=y;
                volatile double result=operation==0 ? left+right : operation==1 ? left-right : left*right;
                return rnd_s(result);
            };
            const double expected0=scalar(a,operation==4 ? a : b);
            const double expected1=scalar(-b,operation==3 ? b : a);
            c.fpscr=0xA0000000;c.cr[1]=7;
            pair_binary[operation](c,target,1,2,record);
            const auto same=[](double actual,double expected) {
                return std::isnan(expected) ? std::isnan(actual) : std::memcmp(&actual,&expected,8)==0;
            };
            CHECK(same(c.f[target],expected0) && same(c.ps1[target],expected1));
            CHECK(c.cr[1]==(record ? 10 : 7) && c.fpscr==0xA0000000);
        }
    // Paired FMA must match two scalar operations, including aliased destinations and fused rounding.
    using PairFma = void (*)(Ctx&,int,int,int,int,bool);
    const PairFma pair_fmas[]={op_ps_madd,op_ps_msub,op_ps_nmadd,op_ps_nmsub,op_ps_madds0,op_ps_madds1};
    const double operands[]={0.0,-0.0,1.0000000596046448,-1.0000001788139343,1e-40,1e20,-1e20,INFINITY,-INFINITY,NAN};
    for(size_t operation=0;operation<6;++operation) for(int target:{0,1,2,3})
        for(double a:operands) for(double b:operands) for(double multiplier:operands) {
            c.f[1]=a;c.ps1[1]=-b;c.f[2]=b;c.ps1[2]=a;c.f[3]=multiplier;c.ps1[3]=-multiplier;
            const auto scalar=[&](double x,double y,double z) {
                volatile double fused=std::fma(x,z,(operation==1 || operation==3) ? -y : y);
                return rnd_s((operation==2 || operation==3) ? -double(fused) : double(fused));
            };
            const double expected0=scalar(a,b,operation==5 ? -multiplier : multiplier);
            const double expected1=scalar(-b,a,operation==4 ? multiplier : -multiplier);
            c.fpscr=0xA0000000;c.cr[1]=0;
            pair_fmas[operation](c,target,1,2,3,true);
            const auto same=[](double actual,double expected) {
                return std::isnan(expected) ? std::isnan(actual) : std::memcmp(&actual,&expected,8)==0;
            };
            CHECK(same(c.f[target],expected0) && same(c.ps1[target],expected1));
            CHECK(c.cr[1]==10);
        }
    // ps_cmpu0 / ps_cmpu1 write a CR field like fcmp
    c.f[1]=1.0;c.ps1[1]=2.0;c.f[2]=10.0;c.ps1[2]=20.0;
    op_ps_cmpu0(c, 2, 1, 2);           CHECK(c.cr[2] == 8);
    op_ps_cmpu1(c, 2, 1, 2);           CHECK(c.cr[2] == 8);
    return 0;
}
