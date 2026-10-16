// Native versions of the hot Dolphin SDK matrix/vector routines (mtx.a). Each one is the translated instruction sequence transcribed onto local
// paired-single values: the same operations in the same order with the same rounding (single results, fused multiply-add), so the guest memory
// it writes and the registers it returns are bit-identical to the emulated psq/ps code, minus the register-file and per-access overhead.
// tests/test_sdk_math_hle.cpp runs the original op sequences against these. Only the registers that carry results (f1) are written back;
// the other floating-point scratch registers are caller-saved by the calling convention.
#include "ppc_rt.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {
using ppc::Ctx;
struct PS { double p0, p1; };

double rs(double v) { return ppc::rnd_s(v); }
// psq_l/psq_st with GQR0: the SDK math code only ever uses float pairs; any other format would change what the code means.
void float_gqr(const Ctx& c) {
    if (((c.gqr[0] >> 16) & 7) >= 4 || (c.gqr[0] & 7) >= 4) throw std::runtime_error("SDK math HLE: GQR0 is not a float format");
}
// The vectors and matrices live in RAM, so these skip the MMIO test and read or write a float pair with one 8-byte access.
#define SDK_INLINE __attribute__((always_inline)) inline
SDK_INLINE double f32_at(Ctx& c, uint32_t ea) { const uint32_t bits = ppc::rd32_ram(c, ea); float f; std::memcpy(&f, &bits, 4); return f; }
SDK_INLINE PS ld(Ctx& c, uint32_t ea) {   // psq_l ..., 0, 0
    uint64_t v; std::memcpy(&v, ppc::hp(c, ea), 8);
    const uint32_t lo = _byteswap_ulong(uint32_t(v)), hi = _byteswap_ulong(uint32_t(v >> 32));
    float x, y; std::memcpy(&x, &lo, 4); std::memcpy(&y, &hi, 4);
    return {x, y};
}
SDK_INLINE PS ld1(Ctx& c, uint32_t ea) { return {f32_at(c, ea), 1.0}; }           // psq_l ..., 1, 0: one element, ps1 = 1.0
SDK_INLINE PS ls(Ctx& c, uint32_t ea) { const double v = f32_at(c, ea); return {v, v}; }   // lfs
SDK_INLINE void st(Ctx& c, uint32_t ea, PS v) {
    const float x = float(v.p0), y = float(v.p1);
    uint32_t lo, hi; std::memcpy(&lo, &x, 4); std::memcpy(&hi, &y, 4);
    const uint64_t pair = uint64_t(_byteswap_ulong(lo)) | uint64_t(_byteswap_ulong(hi)) << 32;
    gx::note_write(ea); if ((ea & 0xFFFu) > 0xFF8u) [[unlikely]] gx::note_write(ea + 7);
    std::memcpy(ppc::hp(c, ea), &pair, 8);
}
SDK_INLINE void st1(Ctx& c, uint32_t ea, PS v) { const float x = float(v.p0); uint32_t bits; std::memcpy(&bits, &x, 4); ppc::wr32_ram(c, ea, bits); }

PS add(PS a, PS b) { return {rs(a.p0 + b.p0), rs(a.p1 + b.p1)}; }
PS sub(PS a, PS b) { return {rs(a.p0 - b.p0), rs(a.p1 - b.p1)}; }
PS mul(PS a, PS b) { return {rs(a.p0 * b.p0), rs(a.p1 * b.p1)}; }
PS muls0(PS a, double s) { return {rs(a.p0 * s), rs(a.p1 * s)}; }                       // ps_muls0: s = ps0 of the scalar operand
// ps_madd*/ps_msub (t, a, b, cc): a * cc + b, a * cc - b, with the multiplier half picked by the variant.
PS madd(PS a, PS cc, PS b) { return {rs(std::fma(a.p0, cc.p0, b.p0)), rs(std::fma(a.p1, cc.p1, b.p1))}; }
PS msub(PS a, PS cc, PS b) { return {rs(std::fma(a.p0, cc.p0, -b.p0)), rs(std::fma(a.p1, cc.p1, -b.p1))}; }
PS madds0(PS a, PS cc, PS b) { return {rs(std::fma(a.p0, cc.p0, b.p0)), rs(std::fma(a.p1, cc.p0, b.p1))}; }
PS madds1(PS a, PS cc, PS b) { return {rs(std::fma(a.p0, cc.p1, b.p0)), rs(std::fma(a.p1, cc.p1, b.p1))}; }
PS sum0(PS a, PS b, PS cc) { return {rs(a.p0 + b.p1), rs(cc.p1)}; }
PS sum1(PS a, PS b, PS cc) { return {rs(cc.p0), rs(a.p0 + b.p1)}; }
PS merge01(PS a, PS b) { return {a.p0, b.p1}; }
PS merge10(PS a, PS b) { return {a.p1, b.p0}; }
PS merge11(PS a, PS b) { return {a.p1, b.p1}; }
PS neg(PS a) { return {-a.p0, -a.p1}; }
PS frsp(double v) { const double r = rs(v); return {r, r}; }
void ret1(Ctx& c, PS v) { c.f[1] = v.p0; c.ps1[1] = v.p1; }
}  // namespace

// void PSMTXIdentity(Mtx m)                          r3
void hle_PSMTXIdentity(ppc::Ctx& c) {
    float_gqr(c);
    const PS zero = ls(c, c.r[2] - 9524), one = ls(c, c.r[2] - 9528);
    const uint32_t m = c.r[3];
    st(c, m + 8, zero);
    const PS row0 = merge10(one, zero), row1 = merge01(zero, one);
    st(c, m + 24, zero); st(c, m + 32, zero); st(c, m + 16, row1); st(c, m + 0, row0); st(c, m + 40, row0);
}

// void PSMTXCopy(Mtx src, Mtx dst)                   r3, r4
void hle_PSMTXCopy(ppc::Ctx& c) {
    float_gqr(c);
    for (uint32_t offset = 0; offset < 48; offset += 8) st(c, c.r[4] + offset, ld(c, c.r[3] + offset));
}

// void PSMTXConcat(Mtx a, Mtx b, Mtx ab)             r3, r4, r5
void hle_PSMTXConcat(ppc::Ctx& c) {
    float_gqr(c);
    const uint32_t a = c.r[3], b = c.r[4], ab = c.r[5];
    const uint32_t k = (uint32_t(int32_t(-32660)) << 16) + 13368u;   // a constant (0, 1) pair in the game's data
    const PS a0 = ld(c, a + 0), b0 = ld(c, b + 0), b1 = ld(c, b + 8), b2 = ld(c, b + 16), a2 = ld(c, a + 16), kk = ld(c, k), b3 = ld(c, b + 24);
    const PS a1 = ld(c, a + 8), a3 = ld(c, a + 24), b4 = ld(c, b + 32), b5 = ld(c, b + 40), a4 = ld(c, a + 32), a5 = ld(c, a + 40);
    PS r12 = muls0(b0, a0.p0), r13 = muls0(b1, a0.p0), r14 = muls0(b0, a2.p0), r15 = muls0(b1, a2.p0);
    r12 = madds1(b2, a0, r12); r14 = madds1(b2, a2, r14); r13 = madds1(b3, a0, r13); r15 = madds1(b3, a2, r15);
    r12 = madds0(b4, a1, r12); r13 = madds0(b5, a1, r13); r14 = madds0(b4, a3, r14); r15 = madds0(b5, a3, r15);
    st(c, ab + 0, r12);
    PS r2 = muls0(b0, a4.p0);
    r13 = madds1(kk, a1, r13);
    PS r0 = muls0(b1, a4.p0);
    st(c, ab + 16, r14);
    r15 = madds1(kk, a3, r15);
    st(c, ab + 8, r13);
    r2 = madds1(b2, a4, r2);
    r0 = madds1(b3, a4, r0);
    r2 = madds0(b4, a5, r2);
    st(c, ab + 24, r15);
    r0 = madds0(b5, a5, r0);
    st(c, ab + 32, r2);
    r0 = madds1(kk, a5, r0);
    st(c, ab + 40, r0);
}

// void PSMTXTransApply(Mtx src, Mtx dst, f32 x, f32 y, f32 z)   r3, r4, f1..f3
void hle_PSMTXTransApply(ppc::Ctx& c) {
    float_gqr(c);
    const uint32_t src = c.r[3], dst = c.r[4];
    const PS tx = frsp(c.f[1]), ty = frsp(c.f[2]), tz = frsp(c.f[3]);
    const PS r4 = ld(c, src + 0), r5 = ld(c, src + 8), r7 = ld(c, src + 24), r8 = ld(c, src + 40);
    st(c, dst + 0, r4);
    const PS o5 = sum1(tx, r5, r5);
    const PS r6 = ld(c, src + 16);
    st(c, dst + 8, o5);
    const PS o7 = sum1(ty, r7, r7);
    const PS r9 = ld(c, src + 32);
    st(c, dst + 16, r6);
    const PS o8 = sum1(tz, r8, r8);
    st(c, dst + 24, o7); st(c, dst + 32, r9); st(c, dst + 40, o8);
}

// void PSMTXScaleApply(Mtx src, Mtx dst, f32 x, f32 y, f32 z)   r3, r4, f1..f3
void hle_PSMTXScaleApply(ppc::Ctx& c) {
    float_gqr(c);
    const uint32_t src = c.r[3], dst = c.r[4];
    const PS sx = frsp(c.f[1]), sy = frsp(c.f[2]), sz = frsp(c.f[3]);
    PS r4 = ld(c, src + 0), r5 = ld(c, src + 8);
    r4 = muls0(r4, sx.p0);
    PS r6 = ld(c, src + 16);
    r5 = muls0(r5, sx.p0);
    PS r7 = ld(c, src + 24);
    r6 = muls0(r6, sy.p0);
    PS r8 = ld(c, src + 32);
    st(c, dst + 0, r4);
    r7 = muls0(r7, sy.p0);
    PS r2 = ld(c, src + 40);
    st(c, dst + 8, r5);
    r8 = muls0(r8, sz.p0);
    st(c, dst + 16, r6);
    r2 = muls0(r2, sz.p0);
    st(c, dst + 24, r7); st(c, dst + 32, r8); st(c, dst + 40, r2);
}

// void PSMTXMultVec(Mtx m, Vec* src, Vec* dst)       r3, r4, r5
void hle_PSMTXMultVec(ppc::Ctx& c) {
    float_gqr(c);
    const uint32_t m = c.r[3], dst = c.r[5];
    const PS v0 = ld(c, c.r[4] + 0), v1 = ld1(c, c.r[4] + 8);
    const PS none{0, 0};   // the dead half of the sum0 destinations is never stored
    PS t = mul(ld(c, m + 0), v0);
    t = madd(ld(c, m + 8), v1, t);
    const PS x = sum0(t, t, none);
    PS u = mul(ld(c, m + 16), v0);
    st1(c, dst + 0, x);
    u = madd(ld(c, m + 24), v1, u);
    const PS y = sum0(u, u, none);
    PS w = mul(ld(c, m + 32), v0);
    st1(c, dst + 4, y);
    w = madd(ld(c, m + 40), v1, w);
    const PS z = sum0(w, w, none);
    st1(c, dst + 8, z);
}

// void PSVECAdd(Vec* a, Vec* b, Vec* ab)             r3, r4, r5
void hle_PSVECAdd(ppc::Ctx& c) {
    float_gqr(c);
    const PS xy = add(ld(c, c.r[3] + 0), ld(c, c.r[4] + 0));
    st(c, c.r[5] + 0, xy);
    st1(c, c.r[5] + 8, add(ld1(c, c.r[3] + 8), ld1(c, c.r[4] + 8)));
}

// void PSVECSubtract(Vec* a, Vec* b, Vec* a_b)       r3, r4, r5
void hle_PSVECSubtract(ppc::Ctx& c) {
    float_gqr(c);
    const PS xy = sub(ld(c, c.r[3] + 0), ld(c, c.r[4] + 0));
    st(c, c.r[5] + 0, xy);
    st1(c, c.r[5] + 8, sub(ld1(c, c.r[3] + 8), ld1(c, c.r[4] + 8)));
}

// void PSVECScale(Vec* src, Vec* dst, f32 scale)     r3, r4, f1
void hle_PSVECScale(ppc::Ctx& c) {
    float_gqr(c);
    const PS xy = ld(c, c.r[3] + 0), z = ld1(c, c.r[3] + 8);
    const double s = c.f[1];
    st(c, c.r[4] + 0, muls0(xy, s));
    st1(c, c.r[4] + 8, muls0(z, s));
}

// void PSVECNormalize(Vec* src, Vec* dst)            r3, r4
void hle_PSVECNormalize(ppc::Ctx& c) {
    float_gqr(c);
    const PS xy = ld(c, c.r[3] + 0), z = ld1(c, c.r[3] + 8);
    const PS sq = mul(xy, xy);
    const double half = f32_at(c, c.r[2] - 9472), three = f32_at(c, c.r[2] - 9468);
    PS len2 = madd(z, z, sq);
    len2 = sum0(len2, sq, z);
    const double estimate = 1.0 / std::sqrt(len2.p0);                       // frsqrte (the emulation computes it exactly)
    double t = rs(estimate * estimate);
    const double h = rs(estimate * half);
    t = rs(-std::fma(t, len2.p0, -three));                                   // fnmsubs: three - t * len2
    const double scale = rs(t * h);
    st(c, c.r[4] + 0, muls0(xy, scale));
    st1(c, c.r[4] + 8, muls0(z, scale));
}

// f32 PSVECSquareMag(Vec* v)                         r3 -> f1
void hle_PSVECSquareMag(ppc::Ctx& c) {
    float_gqr(c);
    const PS xy = mul(ld(c, c.r[3] + 0), ld(c, c.r[3] + 0));
    const PS z = ls(c, c.r[3] + 8);
    ret1(c, sum0(madd(z, z, xy), xy, xy));
}

// f32 PSVECMag(Vec* v)                               r3 -> f1
void hle_PSVECMag(ppc::Ctx& c) {
    float_gqr(c);
    const PS xy = mul(ld(c, c.r[3] + 0), ld(c, c.r[3] + 0));
    const double half = f32_at(c, c.r[2] - 9472), three = f32_at(c, c.r[2] - 9468);
    const PS z = ls(c, c.r[3] + 8);
    const double zero = rs(half - half);
    const PS len2 = sum0(madd(z, z, xy), xy, xy);
    ret1(c, len2);
    if (!(std::isnan(len2.p0) || std::isnan(zero)) && !(len2.p0 < zero) && !(len2.p0 > zero)) return;   // fcmpu equal: the length is zero
    const double estimate = 1.0 / std::sqrt(len2.p0);
    double t = rs(estimate * estimate);
    const double h = rs(estimate * half);
    t = rs(-std::fma(t, len2.p0, -three));
    const double scale = rs(t * h);
    const double mag = rs(len2.p0 * scale);
    ret1(c, {mag, mag});
}

// f32 PSQUATDotProduct(Quaternion* p, Quaternion* q) r3, r4 -> f1
void hle_PSQUATDotProduct(ppc::Ctx& c) {
    float_gqr(c);
    PS hi = ld(c, c.r[3] + 4);
    const PS qhi = ld(c, c.r[4] + 4);
    hi = mul(hi, qhi);
    const PS plo = ld(c, c.r[3] + 0), qlo = ld(c, c.r[4] + 0);
    ret1(c, sum0(madd(plo, qlo, hi), hi, hi));
}

// void PSVECCrossProduct(Vec* a, Vec* b, Vec* axb)   r3, r4, r5
void hle_PSVECCrossProduct(ppc::Ctx& c) {
    float_gqr(c);
    const PS b01 = ld(c, c.r[4] + 0);
    const PS az = ls(c, c.r[3] + 8);
    const PS a01 = ld(c, c.r[3] + 0);
    const PS bswap = merge10(b01, b01);
    const PS bz = ls(c, c.r[4] + 8);
    const PS bz_az = mul(b01, az);
    const PS bx_a = muls0(b01, a01.p0);
    const PS first = msub(a01, bz, bz_az);
    const PS second = msub(a01, bswap, bx_a);
    const PS x = merge11(first, first);
    const PS yz = neg(merge01(first, second));
    st1(c, c.r[5] + 0, x);
    st(c, c.r[5] + 4, yz);
}
