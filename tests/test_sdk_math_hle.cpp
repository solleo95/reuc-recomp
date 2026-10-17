// The native SDK matrix/vector routines must produce bit-identical guest memory and result registers to the translated psq/ps instruction
// sequences. The reference functions below are those sequences (register numbers and order as translated from the game's main.dol).
#include "check.h"
#include "ppc_fixture.h"
#include "gx_write_watch.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <tuple>
#include <vector>
using namespace ppc;

void hle_PSMTXIdentity(Ctx&); void hle_PSMTXCopy(Ctx&); void hle_PSMTXConcat(Ctx&); void hle_PSMTXTransApply(Ctx&); void hle_PSMTXScaleApply(Ctx&);
void hle_PSMTXMultVec(Ctx&); void hle_PSVECAdd(Ctx&); void hle_PSVECSubtract(Ctx&); void hle_PSVECScale(Ctx&); void hle_PSVECNormalize(Ctx&);
void hle_PSVECSquareMag(Ctx&); void hle_PSVECMag(Ctx&); void hle_PSQUATDotProduct(Ctx&); void hle_PSVECCrossProduct(Ctx&); void hle_SegmentTriangleHit(Ctx&); void hle_BoundsRecordInit(Ctx&); void hle_HitBufferConstruct(Ctx&);

namespace {
void ref_PSMTXIdentity(Ctx& c) {
    op_lfs(c, 0, 2, -9524); op_lfs(c, 1, 2, -9528); op_psq_st(c, 0, 3, 8, 0, 0); op_ps_merge10(c, 2, 1, 0, false); op_ps_merge01(c, 1, 0, 1, false);
    op_psq_st(c, 0, 3, 24, 0, 0); op_psq_st(c, 0, 3, 32, 0, 0); op_psq_st(c, 1, 3, 16, 0, 0); op_psq_st(c, 2, 3, 0, 0, 0); op_psq_st(c, 2, 3, 40, 0, 0);
}
void ref_PSMTXCopy(Ctx& c) {
    for (int i = 0; i < 6; ++i) { op_psq_l(c, i, 3, i * 8, 0, 0); op_psq_st(c, i, 4, i * 8, 0, 0); }
}
void ref_PSMTXConcat(Ctx& c) {
    op_psq_l(c, 0, 3, 0, 0, 0); op_psq_l(c, 6, 4, 0, 0, 0); op_addis(c, 6, 0, -32660); op_psq_l(c, 7, 4, 8, 0, 0); op_addi(c, 6, 6, 13368);
    op_psq_l(c, 8, 4, 16, 0, 0); op_ps_muls0(c, 12, 6, 0, false); op_psq_l(c, 2, 3, 16, 0, 0); op_ps_muls0(c, 13, 7, 0, false); op_psq_l(c, 31, 6, 0, 0, 0);
    op_ps_muls0(c, 14, 6, 2, false); op_psq_l(c, 9, 4, 24, 0, 0); op_ps_muls0(c, 15, 7, 2, false); op_psq_l(c, 1, 3, 8, 0, 0);
    op_ps_madds1(c, 12, 8, 12, 0, false); op_psq_l(c, 3, 3, 24, 0, 0); op_ps_madds1(c, 14, 8, 14, 2, false); op_psq_l(c, 10, 4, 32, 0, 0);
    op_ps_madds1(c, 13, 9, 13, 0, false); op_psq_l(c, 11, 4, 40, 0, 0); op_ps_madds1(c, 15, 9, 15, 2, false); op_psq_l(c, 4, 3, 32, 0, 0);
    op_psq_l(c, 5, 3, 40, 0, 0); op_ps_madds0(c, 12, 10, 12, 1, false); op_ps_madds0(c, 13, 11, 13, 1, false); op_ps_madds0(c, 14, 10, 14, 3, false);
    op_ps_madds0(c, 15, 11, 15, 3, false); op_psq_st(c, 12, 5, 0, 0, 0); op_ps_muls0(c, 2, 6, 4, false); op_ps_madds1(c, 13, 31, 13, 1, false);
    op_ps_muls0(c, 0, 7, 4, false); op_psq_st(c, 14, 5, 16, 0, 0); op_ps_madds1(c, 15, 31, 15, 3, false); op_psq_st(c, 13, 5, 8, 0, 0);
    op_ps_madds1(c, 2, 8, 2, 4, false); op_ps_madds1(c, 0, 9, 0, 4, false); op_ps_madds0(c, 2, 10, 2, 5, false); op_psq_st(c, 15, 5, 24, 0, 0);
    op_ps_madds0(c, 0, 11, 0, 5, false); op_psq_st(c, 2, 5, 32, 0, 0); op_ps_madds1(c, 0, 31, 0, 5, false); op_psq_st(c, 0, 5, 40, 0, 0);
}
void ref_PSMTXTransApply(Ctx& c) {
    op_psq_l(c, 4, 3, 0, 0, 0); op_frsp(c, 1, 1, false); op_psq_l(c, 5, 3, 8, 0, 0); op_frsp(c, 2, 2, false); op_psq_l(c, 7, 3, 24, 0, 0); op_frsp(c, 3, 3, false);
    op_psq_l(c, 8, 3, 40, 0, 0); op_psq_st(c, 4, 4, 0, 0, 0); op_ps_sum1(c, 5, 1, 5, 5, false); op_psq_l(c, 6, 3, 16, 0, 0); op_psq_st(c, 5, 4, 8, 0, 0);
    op_ps_sum1(c, 7, 2, 7, 7, false); op_psq_l(c, 9, 3, 32, 0, 0); op_psq_st(c, 6, 4, 16, 0, 0); op_ps_sum1(c, 8, 3, 8, 8, false);
    op_psq_st(c, 7, 4, 24, 0, 0); op_psq_st(c, 9, 4, 32, 0, 0); op_psq_st(c, 8, 4, 40, 0, 0);
}
void ref_PSMTXScaleApply(Ctx& c) {
    op_frsp(c, 1, 1, false); op_psq_l(c, 4, 3, 0, 0, 0); op_frsp(c, 2, 2, false); op_psq_l(c, 5, 3, 8, 0, 0); op_frsp(c, 3, 3, false);
    op_ps_muls0(c, 4, 4, 1, false); op_psq_l(c, 6, 3, 16, 0, 0); op_ps_muls0(c, 5, 5, 1, false); op_psq_l(c, 7, 3, 24, 0, 0); op_ps_muls0(c, 6, 6, 2, false);
    op_psq_l(c, 8, 3, 32, 0, 0); op_psq_st(c, 4, 4, 0, 0, 0); op_ps_muls0(c, 7, 7, 2, false); op_psq_l(c, 2, 3, 40, 0, 0); op_psq_st(c, 5, 4, 8, 0, 0);
    op_ps_muls0(c, 8, 8, 3, false); op_psq_st(c, 6, 4, 16, 0, 0); op_ps_muls0(c, 2, 2, 3, false); op_psq_st(c, 7, 4, 24, 0, 0); op_psq_st(c, 8, 4, 32, 0, 0);
    op_psq_st(c, 2, 4, 40, 0, 0);
}
void ref_PSMTXMultVec(Ctx& c) {
    op_psq_l(c, 0, 4, 0, 0, 0); op_psq_l(c, 2, 3, 0, 0, 0); op_psq_l(c, 1, 4, 8, 1, 0); op_ps_mul(c, 4, 2, 0, false); op_psq_l(c, 3, 3, 8, 0, 0);
    op_ps_madd(c, 5, 3, 4, 1, false); op_psq_l(c, 8, 3, 16, 0, 0); op_ps_sum0(c, 6, 5, 5, 6, false); op_psq_l(c, 9, 3, 24, 0, 0); op_ps_mul(c, 10, 8, 0, false);
    op_psq_st(c, 6, 5, 0, 1, 0); op_ps_madd(c, 11, 9, 10, 1, false); op_psq_l(c, 2, 3, 32, 0, 0); op_ps_sum0(c, 12, 11, 11, 12, false); op_psq_l(c, 3, 3, 40, 0, 0);
    op_ps_mul(c, 4, 2, 0, false); op_psq_st(c, 12, 5, 4, 1, 0); op_ps_madd(c, 5, 3, 4, 1, false); op_ps_sum0(c, 6, 5, 5, 6, false); op_psq_st(c, 6, 5, 8, 1, 0);
}
void ref_PSVECAdd(Ctx& c) {
    op_psq_l(c, 2, 3, 0, 0, 0); op_psq_l(c, 4, 4, 0, 0, 0); op_ps_add(c, 6, 2, 4, false); op_psq_st(c, 6, 5, 0, 0, 0);
    op_psq_l(c, 3, 3, 8, 1, 0); op_psq_l(c, 5, 4, 8, 1, 0); op_ps_add(c, 7, 3, 5, false); op_psq_st(c, 7, 5, 8, 1, 0);
}
void ref_PSVECSubtract(Ctx& c) {
    op_psq_l(c, 2, 3, 0, 0, 0); op_psq_l(c, 4, 4, 0, 0, 0); op_ps_sub(c, 6, 2, 4, false); op_psq_st(c, 6, 5, 0, 0, 0);
    op_psq_l(c, 3, 3, 8, 1, 0); op_psq_l(c, 5, 4, 8, 1, 0); op_ps_sub(c, 7, 3, 5, false); op_psq_st(c, 7, 5, 8, 1, 0);
}
void ref_PSVECScale(Ctx& c) {
    op_psq_l(c, 0, 3, 0, 0, 0); op_psq_l(c, 2, 3, 8, 1, 0); op_ps_muls0(c, 0, 0, 1, false); op_ps_muls0(c, 1, 2, 1, false);
    op_psq_st(c, 0, 4, 0, 0, 0); op_psq_st(c, 1, 4, 8, 1, 0);
}
void ref_PSVECNormalize(Ctx& c) {
    op_psq_l(c, 2, 3, 0, 0, 0); op_psq_l(c, 3, 3, 8, 1, 0); op_ps_mul(c, 5, 2, 2, false); op_lfs(c, 0, 2, -9472); op_lfs(c, 1, 2, -9468);
    op_ps_madd(c, 4, 3, 5, 3, false); op_ps_sum0(c, 4, 4, 5, 3, false); op_frsqrte(c, 5, 4, false); op_fmuls(c, 6, 5, 5, false); op_fmuls(c, 0, 5, 0, false);
    op_fnmsubs(c, 6, 6, 1, 4, false); op_fmuls(c, 5, 6, 0, false); op_ps_muls0(c, 2, 2, 5, false); op_ps_muls0(c, 3, 3, 5, false);
    op_psq_st(c, 2, 4, 0, 0, 0); op_psq_st(c, 3, 4, 8, 1, 0);
}
void ref_PSVECSquareMag(Ctx& c) {
    op_psq_l(c, 0, 3, 0, 0, 0); op_lfs(c, 1, 3, 8); op_ps_mul(c, 0, 0, 0, false); op_ps_madd(c, 1, 1, 0, 1, false); op_ps_sum0(c, 1, 1, 0, 0, false);
}
void ref_PSVECMag(Ctx& c) {
    op_psq_l(c, 0, 3, 0, 0, 0); op_lfs(c, 4, 2, -9472); op_ps_mul(c, 0, 0, 0, false); op_lfs(c, 1, 3, 8); op_fsubs(c, 2, 4, 4, false);
    op_ps_madd(c, 1, 1, 0, 1, false); op_ps_sum0(c, 1, 1, 0, 0, false); op_fcmpu(c, 0, 1, 2);
    if (crbit(c, 2) == true) return;
    op_frsqrte(c, 0, 1, false); op_lfs(c, 3, 2, -9468); op_fmuls(c, 2, 0, 0, false); op_fmuls(c, 0, 0, 4, false); op_fnmsubs(c, 2, 2, 3, 1, false);
    op_fmuls(c, 0, 2, 0, false); op_fmuls(c, 1, 1, 0, false);
}
void ref_PSQUATDotProduct(Ctx& c) {
    op_psq_l(c, 2, 3, 4, 0, 0); op_psq_l(c, 3, 4, 4, 0, 0); op_ps_mul(c, 2, 2, 3, false); op_psq_l(c, 5, 3, 0, 0, 0); op_psq_l(c, 4, 4, 0, 0, 0);
    op_ps_madd(c, 3, 5, 2, 4, false); op_ps_sum0(c, 1, 3, 2, 2, false);
}
void ref_PSVECCrossProduct(Ctx& c) {
    op_psq_l(c, 1, 4, 0, 0, 0); op_lfs(c, 2, 3, 8); op_psq_l(c, 0, 3, 0, 0, 0); op_ps_merge10(c, 6, 1, 1, false); op_lfs(c, 3, 4, 8);
    op_ps_mul(c, 4, 1, 2, false); op_ps_muls0(c, 7, 1, 0, false); op_ps_msub(c, 5, 0, 4, 3, false); op_ps_msub(c, 8, 0, 7, 6, false);
    op_ps_merge11(c, 9, 5, 5, false); op_ps_merge01(c, 10, 5, 8, false); op_psq_st(c, 9, 5, 0, 1, 0); op_ps_neg(c, 10, 10, false); op_psq_st(c, 10, 5, 4, 0, 0);
}

constexpr uint32_t kBase = 0x80010000, kRegion = 0x400, kSda = 0x80020000, kConcatConstant = 0x806C3438;
std::mt19937_64 rng(0x5D1CE);

uint32_t random_float_bits() {
    switch (rng() % 12) {
        case 0: return 0;                                   // +0
        case 1: return 0x80000000u;                         // -0
        case 2: return 0x7F800000u | (uint32_t(rng() & 1) << 31);   // +-inf
        case 3: return 0x7FC00000u | uint32_t(rng() & 0x3FFFFF);    // NaN
        case 4: return uint32_t(rng() & 0x007FFFFF) | (uint32_t(rng() & 1) << 31);   // denormal
        case 5: return 0x7F000000u + uint32_t(rng() & 0x7FFFFF);    // huge
        default: { std::uniform_real_distribution<float> d(-100.f, 100.f); const float v = d(rng); uint32_t b; std::memcpy(&b, &v, 4); return b; }
    }
}
double random_arg() {
    std::uniform_real_distribution<double> d(-50.0, 50.0);
    switch (rng() % 8) { case 0: return 0.0; case 1: return -0.0; case 2: return 1e30; case 3: return 1e-45; default: return d(rng) * (1.0 + 1e-9); }   // not a single: frsp must round it
}
void fill_region(Fix& f, const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i) wr32(f.c, kBase + uint32_t(i) * 4, words[i]);
}
std::vector<uint8_t> region(Fix& f) { return {f.c.mem + (kBase & 0x3FFFFFFF), f.c.mem + (kBase & 0x3FFFFFFF) + kRegion}; }
// The dead stack frames count too: the game reads stale stack slots, so the natives must leave the same bytes there (the window below and just above sp).
void fill_stack(Fix& f, uint32_t sp) { for (uint32_t a = sp - 0x100; a < sp + 0x20; a += 4) wr32(f.c, a, 0xDEAD0000u + (a & 0xFFFFu)); }
void append_stack(std::vector<uint8_t>& memory, Fix& f, uint32_t sp) { const uint8_t* at = f.c.mem + ((sp - 0x100) & 0x3FFFFFFF); memory.insert(memory.end(), at, at + 0x120); }
uint64_t bits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

// Equal bits, except that any two NaNs match: which operand's payload survives x86 NaN propagation is the compiler's choice and never meaningful.
bool is_nan32(uint32_t w) { return (w & 0x7F800000u) == 0x7F800000u && (w & 0x007FFFFFu); }
bool same_memory(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i + 4 <= a.size(); i += 4) {
        uint32_t x, y; std::memcpy(&x, &a[i], 4); std::memcpy(&y, &b[i], 4);
        if (x != y && !(is_nan32(_byteswap_ulong(x)) && is_nan32(_byteswap_ulong(y)))) return false;
    }
    return true;
}
bool same_double(uint64_t x, uint64_t y) { double a, b; std::memcpy(&a, &x, 8); std::memcpy(&b, &y, 8); return x == y || (a != a && b != b); }

struct Result { std::vector<uint8_t> memory; uint64_t f1, ps1_1; };
Result run(Fix& f, void (*fn)(Ctx&), const std::vector<uint32_t>& words, const uint32_t (&args)[3], const double (&fp)[3], const uint32_t (&constants)[6]) {
    Ctx& c = f.c;
    fill_region(f, words);
    wr32(c, kSda - 9524, constants[0]); wr32(c, kSda - 9528, constants[1]); wr32(c, kSda - 9472, constants[2]); wr32(c, kSda - 9468, constants[3]);
    wr32(c, kConcatConstant, constants[4]); wr32(c, kConcatConstant + 4, constants[5]);
    c.r[2] = kSda; c.r[3] = args[0]; c.r[4] = args[1]; c.r[5] = args[2];
    c.f[1] = fp[0]; c.f[2] = fp[1]; c.f[3] = fp[2];
    for (int i = 0; i < 32; ++i) { if (i >= 1 && i <= 3) { c.ps1[i] = fp[i - 1]; continue; } c.f[i] = 123.0 + i; c.ps1[i] = -7.0 - i; }
    c.gqr[0] = 0;
    fn(c);
    return {region(f), bits(c.f[1]), bits(c.ps1[1])};
}
void compare(const char* name, void (*hle)(Ctx&), void (*ref)(Ctx&), bool returns, int iterations = 3000) {
    int mismatches = 0;
    for (int n = 0; n < iterations; ++n) {
        std::vector<uint32_t> words(kRegion / 4);
        for (auto& w : words) w = random_float_bits();
        uint32_t constants[6] = {0, 0x3F800000u, 0x3F000000u, 0x40400000u, random_float_bits(), random_float_bits()};   // 0, 1, 0.5, 3 as in the SDK data
        if (n % 5 == 0) for (auto& k : constants) k = random_float_bits();
        const uint32_t a = kBase, b = kBase + 0x100, ab = kBase + 0x200;
        uint32_t args[3] = {a, b, ab};
        switch (n % 4) { case 1: args[2] = a; break; case 2: args[2] = b; break; case 3: args[0] = kBase + 0x40; args[1] = kBase + 0x40; args[2] = kBase + 0x40; break; }   // in place and fully aliased
        double fp[3] = {random_arg(), random_arg(), random_arg()};
        Fix expected, got;
        const auto want = run(expected, ref, words, args, fp, constants);
        const auto have = run(got, hle, words, args, fp, constants);
        const bool same = same_memory(want.memory, have.memory) && (!returns || (same_double(want.f1, have.f1) && same_double(want.ps1_1, have.ps1_1)));
        if (!same && ++mismatches <= 3) std::printf("%s differs at iteration %d (args %08X %08X %08X)\n", name, n, args[0], args[1], args[2]);
    }
    std::printf("%s: %d mismatches in %d randomised runs\n", name, mismatches, iterations);
    CHECK(mismatches == 0);
}

// The translated instructions of the game's segment/triangle test (800460A0), calling the reference op sequences of the routines it uses.
void ref_SegmentTriangleHit(Ctx& c) {
    const uint32_t entry_sp = c.r[1];
    op_stwu(c, 1, 1, -144); op_mfspr(c, 0, 8); op_stw(c, 0, 1, 148); op_stfd(c, 31, 1, 128); op_psq_st(c, 31, 1, 136, 0, 0); op_stfd(c, 30, 1, 112); op_psq_st(c, 30, 1, 120, 0, 0);
    op_addi(c, 11, 1, 112);
    for (int i = 27; i <= 31; ++i) wr32(c, c.r[11] - 4 * (32 - i), c.r[i]);                  // _savegpr_27
    op_or(c, 6, 30, 6, false); op_or(c, 3, 27, 3, false); op_or(c, 4, 28, 4, false); op_or(c, 5, 29, 5, false); op_or(c, 7, 3, 7, false);
    op_or(c, 8, 31, 8, false); op_or(c, 30, 4, 30, false); op_addi(c, 5, 1, 68);
    ref_PSVECSubtract(c);
    op_or(c, 31, 3, 31, false); op_or(c, 30, 4, 30, false); op_addi(c, 5, 1, 56);
    ref_PSVECSubtract(c);
    op_or(c, 29, 3, 29, false); op_or(c, 28, 4, 28, false); op_addi(c, 5, 1, 44);
    ref_PSVECSubtract(c);
    op_addi(c, 3, 1, 44); op_addi(c, 4, 1, 56); op_addi(c, 5, 1, 32);
    ref_PSVECCrossProduct(c);
    op_addi(c, 3, 1, 68); op_addi(c, 4, 1, 32);
    ref_PSQUATDotProduct(c);
    op_lfs(c, 0, 2, -32296); op_fmr(c, 30, 1, false); op_fcmpo(c, 0, 1, 0);
    if (crbit(c, 0) == false) goto L_8004613C;
    op_addi(c, 3, 0, 0); goto L_80046208;
L_8004613C:;
    op_or(c, 28, 3, 28, false); op_or(c, 30, 4, 30, false); op_addi(c, 5, 1, 20);
    ref_PSVECSubtract(c);
    op_addi(c, 3, 1, 20); op_addi(c, 4, 1, 32);
    ref_PSQUATDotProduct(c);
    op_lfs(c, 0, 2, -32304); op_fmr(c, 31, 1, false); op_fcmpo(c, 0, 1, 0);
    if (crbit(c, 0) == true) goto L_80046170;
    op_fcmpo(c, 0, 1, 30);
    if (crbit(c, 1) == false) goto L_80046178;
L_80046170:;
    op_addi(c, 3, 0, 0); goto L_80046208;
L_80046178:;
    op_addi(c, 3, 1, 20); op_addi(c, 4, 1, 68); op_addi(c, 5, 1, 8);
    ref_PSVECCrossProduct(c);
    op_addi(c, 3, 1, 44); op_addi(c, 4, 1, 8);
    ref_PSQUATDotProduct(c);
    op_lfs(c, 0, 2, -32304); op_fcmpo(c, 0, 1, 0);
    if (crbit(c, 0) == true) goto L_800461AC;
    op_fadds(c, 0, 31, 1, false); op_fcmpo(c, 0, 0, 30);
    if (crbit(c, 1) == false) goto L_800461B4;
L_800461AC:;
    op_addi(c, 3, 0, 0); goto L_80046208;
L_800461B4:;
    op_addi(c, 3, 1, 56); op_addi(c, 4, 1, 8);
    ref_PSQUATDotProduct(c);
    op_lfs(c, 2, 2, -32300); op_lfs(c, 0, 2, -32304); op_fdivs(c, 3, 2, 30, false); op_fmuls(c, 1, 1, 3, false); op_fcmpo(c, 0, 1, 0);
    if (crbit(c, 0) == true) goto L_800461E0;
    op_fcmpo(c, 0, 1, 2);
    if (crbit(c, 1) == false) goto L_800461E8;
L_800461E0:;
    op_addi(c, 3, 0, 0); goto L_80046208;
L_800461E8:;
    op_addi(c, 3, 1, 44); op_or(c, 3, 4, 3, false);
    ref_PSVECScale(c);
    op_or(c, 28, 3, 28, false); op_or(c, 27, 5, 27, false); op_addi(c, 4, 1, 44);
    ref_PSVECAdd(c);
    op_addi(c, 3, 0, 1);
L_80046208:;
    c.r[1] = entry_sp;   // the epilogue (the saved registers and the frame are dead after the return)
}

struct TriResult { std::vector<uint8_t> memory; uint32_t r3; };
TriResult run_tri(Fix& f, void (*fn)(Ctx&), const std::vector<uint32_t>& words, const uint32_t (&arg)[6], const uint32_t (&k)[3]) {
    Ctx& c = f.c;
    fill_region(f, words);
    wr32(c, kSda - 32296, k[0]); wr32(c, kSda - 32304, k[1]); wr32(c, kSda - 32300, k[2]);
    c.r[2] = kSda; c.r[1] = kBase + 0x1000; c.lr = 0x80046000u;
    for (int i = 27; i <= 31; ++i) c.r[i] = 0xBBBB0000u + i;
    fill_stack(f, c.r[1]);
    for (int i = 0; i < 6; ++i) c.r[3 + i] = arg[i];
    for (int i = 0; i < 32; ++i) { c.f[i] = 123.0 + i; c.ps1[i] = -7.0 - i; }
    c.f[1] = 5.0; c.gqr[0] = 0;
    fn(c);
    auto memory = region(f); append_stack(memory, f, kBase + 0x1000);
    return {memory, c.r[3]};
}
uint32_t float_bits(double v) { const float x = float(v); uint32_t b; std::memcpy(&b, &x, 4); return b; }
void compare_segment_triangle(int iterations = 20000) {
    int mismatches = 0, hits = 0;
    for (int n = 0; n < iterations; ++n) {
        std::vector<uint32_t> words(kRegion / 4, 0);
        const uint32_t out = kBase + 0x60, p0 = kBase + 0x10, p1 = kBase + 0x20, v0 = kBase + 0x30, v1 = kBase + 0x40, v2 = kBase + 0x50;
        auto put = [&](uint32_t at, double x, double y, double z) { const size_t i = (at - kBase) / 4; words[i] = float_bits(x); words[i + 1] = float_bits(y); words[i + 2] = float_bits(z); };
        std::uniform_real_distribution<double> d(-10, 10), unit(0.0, 1.0);
        if (n % 3 != 0) {   // a segment through a random triangle (most of these hit), jittered so that edges and parallel cases appear too
            double a[3], b[3], cc[3];
            for (int k = 0; k < 3; ++k) { a[k] = d(rng); b[k] = d(rng); cc[k] = d(rng); }
            double u = unit(rng), v = unit(rng) * (1 - u), nx = (b[1]-a[1])*(cc[2]-a[2]) - (b[2]-a[2])*(cc[1]-a[1]), ny = (b[2]-a[2])*(cc[0]-a[0]) - (b[0]-a[0])*(cc[2]-a[2]), nz = (b[0]-a[0])*(cc[1]-a[1]) - (b[1]-a[1])*(cc[0]-a[0]);
            double px = a[0] + u*(b[0]-a[0]) + v*(cc[0]-a[0]), py = a[1] + u*(b[1]-a[1]) + v*(cc[1]-a[1]), pz = a[2] + u*(b[2]-a[2]) + v*(cc[2]-a[2]);
            const double s0 = unit(rng) * 2 - 0.3, s1 = unit(rng) * 2 - 0.3;
            put(v0, a[0], a[1], a[2]); put(v1, b[0], b[1], b[2]); put(v2, cc[0], cc[1], cc[2]);
            put(p0, px - nx * 0.05 * s0 + d(rng) * 1e-3, py - ny * 0.05 * s0, pz - nz * 0.05 * s0); put(p1, px + nx * 0.05 * s1, py + ny * 0.05 * s1 + d(rng) * 1e-3, pz + nz * 0.05 * s1);
        } else for (auto& w : words) w = random_float_bits();   // arbitrary bit patterns: NaN, inf, denormals
        if(n%19==0) {
            // Determinant at either side of epsilon, barycentric edges, and zero-area triangles.
            const float epsilon=1e-4f;
            const float det=n%5==0 ? std::nextafter(epsilon,0.f) : n%5==1 ? std::nextafter(epsilon,1.f) : epsilon;
            const double x=n%7==0 ? -0.0 : n%7==1 ? 1.0 : n%7==2 ? std::nextafter(1.f,2.f) : .25;
            put(v0,0,0,0); put(v1,1,0,0); put(v2,0,n%11==0 ? 0 : 1,0);
            put(p0,x,0,det); put(p1,x,0,0);
        }
        if (n % 7 == 0) for (uint32_t at : {p0, p1}) { const size_t i = (at - kBase) / 4; words[i + 3] = random_float_bits(); }
        uint32_t arg[6] = {out, p0, p1, v0, v1, v2};
        if (n % 11 == 0) arg[0] = p0; else if (n % 13 == 0) arg[0] = p1;   // the hit point overwrites an input
        uint32_t k[3] = {float_bits(1e-4), 0, float_bits(1.0)};
        if (n % 17 == 0) { k[0] = random_float_bits(); k[1] = random_float_bits(); k[2] = random_float_bits(); }
        Fix expected, got;
        const auto want = run_tri(expected, ref_SegmentTriangleHit, words, arg, k);
        const auto have = run_tri(got, hle_SegmentTriangleHit, words, arg, k);
        hits += want.r3 == 1;
        if (!(same_memory(want.memory, have.memory) && want.r3 == have.r3) && ++mismatches <= 3) std::printf("SegmentTriangleHit differs at iteration %d (reference %u, native %u)\n", n, want.r3, have.r3);
    }
    std::printf("SegmentTriangleHit: %d mismatches in %d randomised runs (%d hits)\n", mismatches, iterations, hits);
    CHECK(mismatches == 0);
    CHECK(hits > iterations / 4);
}

// The translated instructions of the bounds record init (801316C8) and the hit buffer constructor (800462E8); the element constructor (80131684) is
// empty (a lone blr), so __construct_array (80289B34) has no effect on guest memory besides its own dead stack frame.
void ref_BoundsRecordInit(Ctx& c) {
    op_lfs(c, 1, 2, -26752); op_addi(c, 0, 0, 0); op_lfs(c, 0, 2, -26748); op_stfs(c, 1, 3, 0); op_stfs(c, 1, 3, 4); op_stfs(c, 1, 3, 8);
    op_stfs(c, 1, 3, 12); op_stfs(c, 1, 3, 16); op_stfs(c, 1, 3, 20); op_stfs(c, 0, 3, 24); op_stw(c, 0, 3, 28); op_stw(c, 0, 3, 36);
}
// fn_80289B34 (__construct_array, frame -48) for the case of this caller: the element constructor (80131684) is an empty function, and the count
// (r7 = 32) equals the element counter at the end, so the destructor branch is not taken.
void ref_ConstructArray(Ctx& c) {
    op_stwu(c, 1, 1, -48); op_mfspr(c, 0, 8); op_stw(c, 0, 1, 52); op_addi(c, 0, 0, 0); op_stw(c, 31, 1, 44); op_or(c, 3, 31, 3, false); op_stw(c, 30, 1, 40);
    op_or(c, 7, 30, 7, false); op_stw(c, 29, 1, 36); op_or(c, 6, 29, 6, false); op_stw(c, 28, 1, 32); op_or(c, 4, 28, 4, false);
    op_stw(c, 3, 1, 8); op_stw(c, 6, 1, 12); op_stw(c, 7, 1, 16); op_stw(c, 5, 1, 20); op_stw(c, 0, 1, 24);
    for (;;) {
        op_lwz(c, 4, 1, 24); op_cmpl(c, 0, 4, 30);
        if (crbit(c, 0) == false) break;
        ppc::poll(c);
        op_or(c, 28, 12, 28, false); op_or(c, 31, 3, 31, false); op_addi(c, 4, 0, 1);   // the element constructor call (empty) goes here
        op_lwz(c, 3, 1, 24); op_add(c, 31, 31, 29, false); op_addi(c, 0, 3, 1); op_stw(c, 0, 1, 24);
    }
    op_lwz(c, 0, 1, 16); op_cmpl(c, 0, 4, 0);
    CHECK(crbit(c, 0) == false);
    op_lwz(c, 0, 1, 52); op_lwz(c, 31, 1, 44); op_lwz(c, 30, 1, 40); op_lwz(c, 29, 1, 36); op_lwz(c, 28, 1, 32); op_mtspr(c, 0, 8); op_addi(c, 1, 1, 48);
}
void ref_HitBufferConstruct(Ctx& c) {
    const uint32_t entry_sp = c.r[1];
    op_stwu(c, 1, 1, -32); op_mfspr(c, 0, 8); op_addis(c, 6, 0, -32705); op_addis(c, 4, 0, -32749); op_stw(c, 0, 1, 36); op_addis(c, 5, 0, -32749);
    op_addi(c, 6, 6, 7256); op_addi(c, 4, 4, 5764); op_stw(c, 31, 1, 28); op_addi(c, 5, 5, 5768); op_addi(c, 7, 0, 32); op_stw(c, 30, 1, 24);
    op_stw(c, 29, 1, 20); op_or(c, 3, 29, 3, false); op_stw(c, 6, 3, 0); op_addi(c, 6, 0, 108); op_addi(c, 3, 3, 8);
    c.lr = 0x80046330u; ref_ConstructArray(c);   // fn_80289B34: construct 32 elements with an empty constructor
    op_addi(c, 3, 29, 3464);
    /* fn_80131684: empty */
    op_addi(c, 0, 0, 0); op_addi(c, 31, 0, 0); op_stw(c, 0, 29, 4); op_addi(c, 30, 0, 0);
    do {
        op_add(c, 3, 29, 30, false); op_addi(c, 3, 3, 8);
        ref_BoundsRecordInit(c);
        op_addi(c, 31, 31, 1); op_addi(c, 30, 30, 108); op_cmpli(c, 0, 31, 32u);
        if (crbit(c, 0) == true) ppc::poll(c);
    } while (crbit(c, 0) == true);
    op_addi(c, 3, 29, 3464);
    ref_BoundsRecordInit(c);
    op_addi(c, 0, 0, 0); op_or(c, 29, 3, 29, false); op_stw(c, 0, 29, 3572);
    op_lwz(c, 31, 1, 28); op_lwz(c, 30, 1, 24); op_lwz(c, 29, 1, 20); op_lwz(c, 0, 1, 36); op_mtspr(c, 0, 8);
    c.r[1] = entry_sp;
}

void compare_hit_buffer(int iterations = 3000) {
    int mismatches = 0;
    for (int n = 0; n < iterations; ++n) {
        std::vector<uint32_t> words(0x1000 / 4);
        for (auto& w : words) w = random_float_bits();
        uint32_t constants[2] = {n % 9 ? float_bits(1e30) : random_float_bits(), n % 7 ? 0xC0000000u : random_float_bits()};
        const uint32_t this_ptr = kBase + 0x40 + (n % 5) * 4;
        for (int which = 0; which < 2; ++which) {
            void (*ref)(Ctx&) = which ? ref_BoundsRecordInit : ref_HitBufferConstruct;
            void (*hle)(Ctx&) = which ? hle_BoundsRecordInit : hle_HitBufferConstruct;
            Fix a, b;
            auto go = [&](Fix& f, void (*fn)(Ctx&)) {   // BoundsRecordInit has no frame: its stack window just stays as filled
                Ctx& c = f.c;
                for (size_t i = 0; i < words.size(); ++i) wr32(c, kBase + uint32_t(i) * 4, words[i]);
                wr32(c, kSda - 26752, constants[0]); wr32(c, kSda - 26748, constants[1]);
                c.r[2] = kSda; c.r[1] = kBase + 0x3000; c.r[3] = this_ptr; c.lr = 0x80012345u; fill_stack(f, c.r[1]); c.r[28] = 0xAAAA0028; c.r[29] = 0xAAAA0029; c.r[30] = 0xAAAA0030; c.r[31] = 0xAAAA0031;
                c.poll_count = uint32_t(n);
                for (int i = 0; i < 32; ++i) { c.f[i] = 123.0 + i; c.ps1[i] = -7.0 - i; }
                fn(c);
                std::vector<uint8_t> memory(f.c.mem + (kBase & 0x3FFFFFFF), f.c.mem + (kBase & 0x3FFFFFFF) + 0x1000);
                append_stack(memory, f, kBase + 0x3000);
                return std::make_tuple(memory, c.r[3], c.r[29], c.r[30], c.r[31], bits(c.f[1]), bits(c.f[0]), bits(c.ps1[1]), c.poll_count);
            };
            const auto want = go(a, ref);
            const auto have = go(b, hle);
            const bool same = same_memory(std::get<0>(want), std::get<0>(have)) && std::get<1>(want) == std::get<1>(have) && std::get<2>(want) == std::get<2>(have)
                && std::get<3>(want) == std::get<3>(have) && std::get<4>(want) == std::get<4>(have) && std::get<8>(want) == std::get<8>(have) && (which == 0 || (std::get<5>(want) == std::get<5>(have) && std::get<6>(want) == std::get<6>(have) && std::get<7>(want) == std::get<7>(have)));
            if (!same && ++mismatches <= 3) std::printf("%s differs at iteration %d\n", which ? "BoundsRecordInit" : "HitBufferConstruct", n);
        }
    }
    std::printf("BoundsRecordInit / HitBufferConstruct: %d mismatches in %d randomised runs each\n", mismatches, iterations);
    CHECK(mismatches == 0);
}
}  // namespace

int main() {
    {
        Fix fix;
        fix.c.r[3]=0x80010000; fix.c.r[4]=0x80010100; fix.c.r[5]=0x80010FF0;
        const auto stamp=gx::watch_stamp();
        hle_PSMTXConcat(fix.c);
        CHECK(gx::written_since(0x80010FF0,16,stamp));
        CHECK(gx::written_since(0x80011000,32,stamp));
        CHECK(!gx::written_since(0x80012000,4,stamp));
    }
    compare("PSMTXIdentity", hle_PSMTXIdentity, ref_PSMTXIdentity, false);
    compare("PSMTXCopy", hle_PSMTXCopy, ref_PSMTXCopy, false);
    compare("PSMTXConcat", hle_PSMTXConcat, ref_PSMTXConcat, false);
    compare("PSMTXTransApply", hle_PSMTXTransApply, ref_PSMTXTransApply, false);
    compare("PSMTXScaleApply", hle_PSMTXScaleApply, ref_PSMTXScaleApply, false);
    compare("PSMTXMultVec", hle_PSMTXMultVec, ref_PSMTXMultVec, false);
    compare("PSVECAdd", hle_PSVECAdd, ref_PSVECAdd, false);
    compare("PSVECSubtract", hle_PSVECSubtract, ref_PSVECSubtract, false);
    compare("PSVECScale", hle_PSVECScale, ref_PSVECScale, false);
    compare("PSVECNormalize", hle_PSVECNormalize, ref_PSVECNormalize, false);
    compare("PSVECSquareMag", hle_PSVECSquareMag, ref_PSVECSquareMag, true);
    compare("PSVECMag", hle_PSVECMag, ref_PSVECMag, true);
    compare("PSQUATDotProduct", hle_PSQUATDotProduct, ref_PSQUATDotProduct, true);
    compare("PSVECCrossProduct", hle_PSVECCrossProduct, ref_PSVECCrossProduct, false);
    compare_segment_triangle();
    compare_hit_buffer();
    return 0;
}
