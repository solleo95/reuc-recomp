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
using DoublePair = double __attribute__((ext_vector_type(2)));
using FloatPair = float __attribute__((ext_vector_type(2)));
using BytePair = uint8_t __attribute__((ext_vector_type(8)));

double rs(double v) { return ppc::rnd_s(v); }
// psq_l/psq_st with GQR0: the SDK math code only ever uses float pairs; any other format would change what the code means.
void float_gqr(const Ctx& c) {
    if (((c.gqr[0] >> 16) & 7) >= 4 || (c.gqr[0] & 7) >= 4) throw std::runtime_error("SDK math HLE: GQR0 is not a float format");
}
// The vectors and matrices live in RAM, so these skip the MMIO test and read or write a float pair with one 8-byte access.
#define SDK_INLINE __attribute__((always_inline)) inline
SDK_INLINE double f32_at(Ctx& c, uint32_t ea) { const uint32_t bits = ppc::rd32_ram(c, ea); float f; std::memcpy(&f, &bits, 4); return f; }
SDK_INLINE PS ld(Ctx& c, uint32_t ea) {   // psq_l ..., 0, 0
    BytePair bytes; std::memcpy(&bytes,ppc::hp(c,ea),8);
    bytes=__builtin_shufflevector(bytes,bytes,3,2,1,0,7,6,5,4);
    const auto values=__builtin_convertvector(__builtin_bit_cast(FloatPair,bytes),DoublePair);
    return {values[0],values[1]};
}
SDK_INLINE PS ld1(Ctx& c, uint32_t ea) { return {f32_at(c, ea), 1.0}; }           // psq_l ..., 1, 0: one element, ps1 = 1.0
SDK_INLINE PS ls(Ctx& c, uint32_t ea) { const double v = f32_at(c, ea); return {v, v}; }   // lfs
SDK_INLINE void st_unwatched(Ctx& c, uint32_t ea, PS v) {
    const auto floats=__builtin_convertvector(DoublePair{v.p0,v.p1},FloatPair);
    const auto bytes=__builtin_bit_cast(BytePair,floats);
    const auto encoded=__builtin_shufflevector(bytes,bytes,3,2,1,0,7,6,5,4);
    std::memcpy(ppc::hp(c,ea),&encoded,8);
}
SDK_INLINE void st(Ctx& c, uint32_t ea, PS v) {
    gx::note_write(ea); if ((ea & 0xFFFu) > 0xFF8u) [[unlikely]] gx::note_write(ea + 7);
    st_unwatched(c,ea,v);
}
SDK_INLINE void st1(Ctx& c, uint32_t ea, PS v) { const float x = float(v.p0); uint32_t bits; std::memcpy(&bits, &x, 4); ppc::wr32_ram(c, ea, bits); }

SDK_INLINE DoublePair pair(PS v) { return {v.p0,v.p1}; }
// Both lanes retain the PPC double intermediate followed by single-precision rounding.
SDK_INLINE PS rounded(DoublePair v) {
    const auto result=__builtin_convertvector(__builtin_convertvector(v,FloatPair),DoublePair);
    return {result[0],result[1]};
}
PS add(PS a, PS b) { return rounded(pair(a)+pair(b)); }
PS sub(PS a, PS b) { return rounded(pair(a)-pair(b)); }
PS mul(PS a, PS b) { return rounded(pair(a)*pair(b)); }
PS muls0(PS a, double s) { return rounded(pair(a)*DoublePair{s,s}); }                       // ps_muls0: s = ps0 of the scalar operand
// ps_madd*/ps_msub (t, a, b, cc): a * cc + b, a * cc - b, with the multiplier half picked by the variant.
PS madd(PS a, PS cc, PS b) { return rounded(__builtin_elementwise_fma(pair(a),pair(cc),pair(b))); }
PS msub(PS a, PS cc, PS b) { return rounded(__builtin_elementwise_fma(pair(a),pair(cc),-pair(b))); }
PS madds0(PS a, PS cc, PS b) { return rounded(__builtin_elementwise_fma(pair(a),DoublePair{cc.p0,cc.p0},pair(b))); }
PS madds1(PS a, PS cc, PS b) { return rounded(__builtin_elementwise_fma(pair(a),DoublePair{cc.p1,cc.p1},pair(b))); }
PS sum0(PS a, PS b, PS cc) { return {rs(a.p0 + b.p1), rs(cc.p1)}; }
PS sum1(PS a, PS b, PS cc) { return {rs(cc.p0), rs(a.p0 + b.p1)}; }
PS merge01(PS a, PS b) { return {a.p0, b.p1}; }
PS merge10(PS a, PS b) { return {a.p1, b.p0}; }
PS merge11(PS a, PS b) { return {a.p1, b.p1}; }
PS neg(PS a) { return {-a.p0, -a.p1}; }
PS frsp(double v) { const double r = rs(v); return {r, r}; }
void ret1(Ctx& c, PS v) { c.f[1] = v.p0; c.ps1[1] = v.p1; }
// The stack frames of the translated bodies are dead after the return, but the game reads stale stack slots (uninitialised locals), so the natives that
// replace functions with a frame write the same bytes the instruction sequence would (no write barrier: nothing on the stack feeds the GPU).
SDK_INLINE bool in_ram(uint32_t ea, uint32_t size) { return (ea >= 0x80000000u && ea + size <= 0x81800000u) || (ea >= 0x90000000u && ea + size <= 0x94000000u); }
SDK_INLINE void put32(uint8_t* p, uint32_t v) { v = _byteswap_ulong(v); std::memcpy(p, &v, 4); }
SDK_INLINE void put_f32(uint8_t* p, double v) { const float f = float(v); uint32_t b; std::memcpy(&b, &f, 4); put32(p, b); }   // stfs / one half of psq_st
SDK_INLINE void put_f64(uint8_t* p, double v) { uint64_t b; std::memcpy(&b, &v, 8); b = _byteswap_uint64(b); std::memcpy(p, &b, 8); }   // stfd
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
    // No guest safe point between these stores: one barrier covers the complete matrix.
    gx::note_write_range(ab,48);
    st_unwatched(c, ab + 0, r12);
    PS r2 = muls0(b0, a4.p0);
    r13 = madds1(kk, a1, r13);
    PS r0 = muls0(b1, a4.p0);
    st_unwatched(c, ab + 16, r14);
    r15 = madds1(kk, a3, r15);
    st_unwatched(c, ab + 8, r13);
    r2 = madds1(b2, a4, r2);
    r0 = madds1(b3, a4, r0);
    r2 = madds0(b4, a5, r2);
    st_unwatched(c, ab + 24, r15);
    r0 = madds0(b5, a5, r0);
    st_unwatched(c, ab + 32, r2);
    r0 = madds1(kk, a5, r0);
    st_unwatched(c, ab + 40, r0);
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

// int SegmentTriangleHit(Vec* hit, Vec* p0, Vec* p1, Vec* v0, Vec* v1, Vec* v2)  r3..r8 -> r3
// The game's collision test at 800460A0 (a Moller-Trumbore segment p0->p1 against the triangle v0 v1 v2; the hit point goes to *hit). The translated
// function is eight calls of the routines above on stack temporaries; this runs the same operations on locals: identical single-precision
// rounding, identical comparisons (a NaN fails every comparison like fcmpo), only the dead stack temporaries are not written.
// The thresholds are small-data constants (eps, 0, 1). tests/test_sdk_math_hle.cpp runs the original instruction sequence against it.
namespace {
struct V3 { PS xy; double z; };
SDK_INLINE V3 vld(Ctx& c, uint32_t ea) { return {ld(c, ea), f32_at(c, ea + 8)}; }
SDK_INLINE V3 vsub(const V3& a, const V3& b) { return {sub(a.xy, b.xy), rs(a.z - b.z)}; }
SDK_INLINE V3 vcross(const V3& a, const V3& b) {   // PSVECCrossProduct on locals
    const PS az = {a.z, a.z}, bz = {b.z, b.z}, bswap = merge10(b.xy, b.xy);
    const PS bz_az = mul(b.xy, az);
    const PS bx_a = muls0(b.xy, a.xy.p0);
    const PS first = msub(a.xy, bz, bz_az);
    const PS second = msub(a.xy, bswap, bx_a);
    const PS yz = neg(merge01(first, second));
    return {{first.p1, yz.p0}, yz.p1};
}
SDK_INLINE double vdot(const V3& a, const V3& b) {   // the "PSQUATDotProduct" the SDK data labels it: x*x' + y*y' + z*z' with the paired-single rounding
    const PS hi = mul(PS{a.xy.p1, a.z}, PS{b.xy.p1, b.z});
    return sum0(madd(a.xy, b.xy, hi), hi, hi).p0;
}
}  // namespace
void hle_SegmentTriangleHit(ppc::Ctx& c) {
    float_gqr(c);
    const double eps = f32_at(c, c.r[2] - 32296), zero = f32_at(c, c.r[2] - 32304), one = f32_at(c, c.r[2] - 32300);
    const uint32_t out = c.r[3];
    const V3 p0 = vld(c, c.r[4]), p1 = vld(c, c.r[5]), v0 = vld(c, c.r[6]), v1 = vld(c, c.r[7]), v2 = vld(c, c.r[8]);
    const V3 e1 = vsub(v1, v0), e2 = vsub(v2, v0), dir = vsub(p1, p0);
    const V3 pvec = vcross(dir, e2);
    const double det = vdot(e1, pvec);
    c.r[3] = 0;
    // The frame of the translated body (-144): back chain, link register, f30/f31 and r27..r31 saved, then the vector temporaries as far as the
    // run gets. The restore of f30/f31 through psq_l leaves ps1 rounded to a single.
    const uint32_t entry = c.r[1];
    uint8_t* const fr = in_ram(entry - 144, 152) ? ppc::hp(c, entry - 144) : nullptr;
    auto temp = [fr](uint32_t at, const V3& v) { if (fr) { put_f32(fr + at, v.xy.p0); put_f32(fr + at + 4, v.xy.p1); put_f32(fr + at + 8, v.z); } };
    if (fr) {
        put32(fr, entry); put32(fr + 148, c.lr);
        put_f64(fr + 128, c.f[31]); put_f32(fr + 136, c.f[31]); put_f32(fr + 140, c.ps1[31]);
        put_f64(fr + 112, c.f[30]); put_f32(fr + 120, c.f[30]); put_f32(fr + 124, c.ps1[30]);
        for (int i = 27; i <= 31; ++i) put32(fr + 92 + 4 * (i - 27), c.r[i]);
    }
    c.ps1[31] = double(float(c.ps1[31])); c.ps1[30] = double(float(c.ps1[30]));
    temp(68, e1); temp(56, e2); temp(44, dir); temp(32, pvec);
    if (det < eps) return;
    const V3 tvec = vsub(p0, v0);
    temp(20, tvec);
    const double u = vdot(tvec, pvec);
    if (u < zero || u > det) return;
    const V3 qvec = vcross(tvec, e1);
    temp(8, qvec);
    const double v = vdot(dir, qvec);
    if (v < zero || rs(u + v) > det) return;
    const double t = rs(vdot(e2, qvec) * rs(one / det));
    if (t < zero || t > one) return;
    const V3 hit = {muls0(dir.xy, t), muls0(PS{dir.z, 1.0}, t).p0};
    temp(44, hit);   // PSVECScale(dir, dir, t) in place
    const PS xy = add(p0.xy, hit.xy);
    st(c, out + 0, xy);
    st1(c, out + 8, add(PS{p0.z, 1.0}, PS{hit.z, 1.0}));
    c.r[3] = 1;
}

// Two tiny routines of the game's collision code that run thousands of times a frame (together ~7% of the heavy window), written on host memory.
// 801316C8: initialises the 40-byte bounds record at r3 (six floats f1, one float f0, two zero words). 800462E8: the constructor of the 3.6 KB hit
// buffer at r3 (vtable, then __construct_array with an empty element constructor, then this init on 33 records of 108 bytes); it returns r3.
// Both stay on the original per-store path when the record is not entirely in RAM. tests/test_sdk_math_hle.cpp runs the translated instructions.
namespace {
SDK_INLINE uint32_t through_double(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); const float g = float(double(f)); uint32_t r; std::memcpy(&r, &g, 4); return r; }   // lfs then stfs
SDK_INLINE void bounds_record(uint8_t* p, uint32_t low, uint32_t high) {
    for (int i = 0; i < 6; ++i) put32(p + 4 * i, low);
    put32(p + 24, high); put32(p + 28, 0); put32(p + 36, 0);
}
SDK_INLINE void bounds_record_slow(Ctx& c, uint32_t ea, uint32_t low, uint32_t high) {
    for (int i = 0; i < 6; ++i) ppc::wr32(c, ea + 4 * i, low);
    ppc::wr32(c, ea + 24, high); ppc::wr32(c, ea + 28, 0); ppc::wr32(c, ea + 36, 0);
}
}  // namespace
void hle_BoundsRecordInit(ppc::Ctx& c) {
    const uint32_t low = through_double(ppc::rd32_ram(c, c.r[2] - 26752)), high = through_double(ppc::rd32_ram(c, c.r[2] - 26748)), ea = c.r[3];
    if (in_ram(ea, 40)) { gx::note_write_range(ea, 40); bounds_record(ppc::hp(c, ea), low, high); } else bounds_record_slow(c, ea, low, high);
    float f; std::memcpy(&f, &low, 4); c.f[1] = c.ps1[1] = f; std::memcpy(&f, &high, 4); c.f[0] = c.ps1[0] = f; c.r[0] = 0;
}
void hle_HitBufferConstruct(ppc::Ctx& c) {
    const uint32_t low = through_double(ppc::rd32_ram(c, c.r[2] - 26752)), high = through_double(ppc::rd32_ram(c, c.r[2] - 26748)), ea = c.r[3];
    const uint32_t vtable = uint32_t(int32_t(-32705) << 16) + 7256, size = 3576;
    // Dead frames of the translated bodies: this constructor's (-32: back chain, link register, r29..r31) and __construct_array's (80289B34, -48:
    // r28..r31, its arguments and the element counter, which ends at 32), 88 bytes below the caller's stack pointer in all.
    if (const uint32_t entry = c.r[1]; in_ram(entry - 80, 88)) {
        uint8_t* const f = ppc::hp(c, entry - 80);
        put32(f, entry - 32); put32(f + 8, ea + 8); put32(f + 12, 108); put32(f + 16, 32); put32(f + 20, 0x80131688u); put32(f + 24, 32);
        put32(f + 32, c.r[28]); put32(f + 36, ea); put32(f + 40, c.r[30]); put32(f + 44, c.r[31]); put32(f + 52, 0x80046330u);
        put32(f + 48, entry); put32(f + 68, c.r[29]); put32(f + 72, c.r[30]); put32(f + 76, c.r[31]); put32(f + 84, c.lr);
    }
    // Keep the translated constructor's backward-branch polls: they set later safe-point timing and the deterministic clock reads.
    for (uint32_t i = 0; i < 32; ++i) ppc::poll(c);
    if (in_ram(ea, size)) {
        gx::note_write_range(ea, size);
        uint8_t* p = ppc::hp(c, ea);
        put32(p, vtable); put32(p + 4, 0);
        for (uint32_t i = 0; i < 32; ++i) { bounds_record(p + 8 + 108 * i, low, high); if (i < 31) ppc::poll(c); }
        bounds_record(p + 3464, low, high);
        put32(p + 3572, 0);
    } else {
        ppc::wr32(c, ea, vtable); ppc::wr32(c, ea + 4, 0);
        for (uint32_t i = 0; i < 32; ++i) { bounds_record_slow(c, ea + 8 + 108 * i, low, high); if (i < 31) ppc::poll(c); }
        bounds_record_slow(c, ea + 3464, low, high);
        ppc::wr32(c, ea + 3572, 0);
    }
    float f; std::memcpy(&f, &low, 4); c.f[1] = c.ps1[1] = f; std::memcpy(&f, &high, 4); c.f[0] = c.ps1[0] = f; c.r[0] = 0;
}
