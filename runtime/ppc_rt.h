#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>

#define PPC_INLINE __attribute__((always_inline)) inline

#ifdef UC_TRACE
namespace ppc { void trace_enter(uint32_t addr); }
#define UC_ENTER(a) ppc::trace_enter(a)
#else
#define UC_ENTER(a) ((void)0)
#endif

namespace ppc {

struct Ctx {
    uint32_t r[32];
    double f[32];      // FPR / ps0
    double ps1[32];    // ps1
    uint8_t cr[8];     // per field: LT=8 GT=4 EQ=2 SO=1
    uint8_t xer_ca, xer_ov, xer_so;
    uint32_t lr, ctr, msr, fpscr;
    uint32_t gqr[8];
    uint32_t spr[1024];  // SPRs without a dedicated field
    uint32_t reserve_addr;
    uint32_t poll_count;   // backward branches taken; every 2048th one is a safe point
    uint8_t* mem;
};

uint32_t mmio_read(uint32_t ea, int size);
void mmio_write(uint32_t ea, int size, uint32_t v);
[[noreturn]] void trap(uint32_t addr, const char* what);
uint64_t timebase();
void poll_slow(Ctx& c);   // safe point: pending interrupts/timers run here
// Translated code calls this on every backward branch, so a guest spin loop still sees interrupt-driven state changes.
inline void poll(Ctx& c) { if ((++c.poll_count & 0x7FFu) == 0) [[unlikely]] poll_slow(c); }

// ---- memory ----
PPC_INLINE bool is_mmio(uint32_t ea) { return ea - 0xCC000000u < 0x02000000u; }
PPC_INLINE uint8_t* hp(Ctx& c, uint32_t ea) { return c.mem + (ea & 0x3FFFFFFFu); }

PPC_INLINE uint32_t rd32(Ctx& c, uint32_t ea) {
    if (is_mmio(ea)) [[unlikely]] return mmio_read(ea, 4);
    uint32_t v; std::memcpy(&v, hp(c, ea), 4); return _byteswap_ulong(v);
}
PPC_INLINE uint32_t rd16(Ctx& c, uint32_t ea) {
    if (is_mmio(ea)) [[unlikely]] return mmio_read(ea, 2);
    uint16_t v; std::memcpy(&v, hp(c, ea), 2); return _byteswap_ushort(v);
}
PPC_INLINE uint32_t rd8(Ctx& c, uint32_t ea) {
    if (is_mmio(ea)) [[unlikely]] return mmio_read(ea, 1);
    return *hp(c, ea);
}
PPC_INLINE void wr32(Ctx& c, uint32_t ea, uint32_t v) {
    if (is_mmio(ea)) [[unlikely]] { mmio_write(ea, 4, v); return; }
    v = _byteswap_ulong(v); std::memcpy(hp(c, ea), &v, 4);
}
PPC_INLINE void wr16(Ctx& c, uint32_t ea, uint32_t v) {
    if (is_mmio(ea)) [[unlikely]] { mmio_write(ea, 2, v & 0xFFFF); return; }
    uint16_t h = _byteswap_ushort(uint16_t(v)); std::memcpy(hp(c, ea), &h, 2);
}
PPC_INLINE void wr8(Ctx& c, uint32_t ea, uint32_t v) {
    if (is_mmio(ea)) [[unlikely]] { mmio_write(ea, 1, v & 0xFF); return; }
    *hp(c, ea) = uint8_t(v);
}
PPC_INLINE float rdf32(Ctx& c, uint32_t ea) { uint32_t b = rd32(c, ea); float f; std::memcpy(&f, &b, 4); return f; }
PPC_INLINE double rdf64(Ctx& c, uint32_t ea) {
    if (is_mmio(ea)) [[unlikely]] { uint64_t hi = mmio_read(ea, 4), lo = mmio_read(ea + 4, 4); uint64_t b = hi << 32 | lo; double d; std::memcpy(&d, &b, 8); return d; }
    uint64_t v; std::memcpy(&v, hp(c, ea), 8); v = _byteswap_uint64(v); double d; std::memcpy(&d, &v, 8); return d;
}
PPC_INLINE void wrf32(Ctx& c, uint32_t ea, float v) { uint32_t b; std::memcpy(&b, &v, 4); wr32(c, ea, b); }
PPC_INLINE void wrf64(Ctx& c, uint32_t ea, double v) {
    uint64_t b; std::memcpy(&b, &v, 8);
    if (is_mmio(ea)) [[unlikely]] { mmio_write(ea, 4, uint32_t(b >> 32)); mmio_write(ea + 4, 4, uint32_t(b)); return; }
    b = _byteswap_uint64(b); std::memcpy(hp(c, ea), &b, 8);
}

PPC_INLINE uint32_t ea_d(Ctx& c, int a, int32_t d) { return (a ? c.r[a] : 0u) + uint32_t(d); }
PPC_INLINE uint32_t ea_x(Ctx& c, int a, int b) { return (a ? c.r[a] : 0u) + c.r[b]; }

// ---- condition register ----
PPC_INLINE void cr0(Ctx& c, uint32_t v) { int32_t s = int32_t(v); c.cr[0] = uint8_t((s < 0 ? 8 : s > 0 ? 4 : 2) | c.xer_so); }
PPC_INLINE bool crbit(Ctx& c, int bit) { return (c.cr[bit >> 2] >> (3 - (bit & 3))) & 1; }
PPC_INLINE void set_crbit(Ctx& c, int bit, bool v) {
    uint8_t m = uint8_t(1 << (3 - (bit & 3)));
    c.cr[bit >> 2] = uint8_t(v ? (c.cr[bit >> 2] | m) : (c.cr[bit >> 2] & ~m));
}
PPC_INLINE void op_crand(Ctx& c, int t, int a, int b) { set_crbit(c, t, crbit(c, a) & crbit(c, b)); }
PPC_INLINE void op_crandc(Ctx& c, int t, int a, int b) { set_crbit(c, t, crbit(c, a) & !crbit(c, b)); }
PPC_INLINE void op_cror(Ctx& c, int t, int a, int b) { set_crbit(c, t, crbit(c, a) | crbit(c, b)); }
PPC_INLINE void op_crorc(Ctx& c, int t, int a, int b) { set_crbit(c, t, crbit(c, a) | !crbit(c, b)); }
PPC_INLINE void op_crxor(Ctx& c, int t, int a, int b) { set_crbit(c, t, crbit(c, a) ^ crbit(c, b)); }
PPC_INLINE void op_crnand(Ctx& c, int t, int a, int b) { set_crbit(c, t, !(crbit(c, a) & crbit(c, b))); }
PPC_INLINE void op_crnor(Ctx& c, int t, int a, int b) { set_crbit(c, t, !(crbit(c, a) | crbit(c, b))); }
PPC_INLINE void op_creqv(Ctx& c, int t, int a, int b) { set_crbit(c, t, !(crbit(c, a) ^ crbit(c, b))); }
PPC_INLINE void op_mcrf(Ctx& c, int d, int s) { c.cr[d] = c.cr[s]; }
PPC_INLINE void op_mfcr(Ctx& c, int t) { uint32_t v = 0; for (int i = 0; i < 8; ++i) v = v << 4 | c.cr[i]; c.r[t] = v; }
PPC_INLINE void op_mtcrf(Ctx& c, int crm, int rs) {
    for (int i = 0; i < 8; ++i) if (crm & (0x80 >> i)) c.cr[i] = uint8_t((c.r[rs] >> (28 - 4 * i)) & 0xF);
}
PPC_INLINE void op_mcrxr(Ctx& c, int d) {
    c.cr[d] = uint8_t(c.xer_so << 3 | c.xer_ov << 2 | c.xer_ca << 1); c.xer_so = c.xer_ov = c.xer_ca = 0;
}

// ---- integer arithmetic (OE forms are not implemented; the translator traps on them) ----
PPC_INLINE void rc_set(Ctx& c, uint32_t v, bool rc) { if (rc) cr0(c, v); }
PPC_INLINE void op_addi(Ctx& c, int d, int a, int32_t i) { c.r[d] = (a ? c.r[a] : 0u) + uint32_t(i); }
PPC_INLINE void op_addis(Ctx& c, int d, int a, int32_t i) { c.r[d] = (a ? c.r[a] : 0u) + (uint32_t(i) << 16); }
PPC_INLINE void op_addic(Ctx& c, int d, int a, int32_t i) {
    uint32_t x = c.r[a], r = x + uint32_t(i); c.xer_ca = r < x; c.r[d] = r;
}
PPC_INLINE void op_addic_(Ctx& c, int d, int a, int32_t i) { op_addic(c, d, a, i); cr0(c, c.r[d]); }
PPC_INLINE void op_subfic(Ctx& c, int d, int a, int32_t i) {
    uint32_t x = c.r[a]; c.xer_ca = uint32_t(i) >= x; c.r[d] = uint32_t(i) - x;
}
PPC_INLINE void op_add(Ctx& c, int d, int a, int b, bool rc) { c.r[d] = c.r[a] + c.r[b]; rc_set(c, c.r[d], rc); }
PPC_INLINE void op_addc(Ctx& c, int d, int a, int b, bool rc) {
    uint32_t x = c.r[a], y = c.r[b], r = x + y; c.xer_ca = r < x; c.r[d] = r; rc_set(c, r, rc);
}
PPC_INLINE void op_adde(Ctx& c, int d, int a, int b, bool rc) {
    uint64_t s = uint64_t(c.r[a]) + c.r[b] + c.xer_ca; c.xer_ca = uint8_t(s >> 32); c.r[d] = uint32_t(s); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_addze(Ctx& c, int d, int a, bool rc) {
    uint64_t s = uint64_t(c.r[a]) + c.xer_ca; c.xer_ca = uint8_t(s >> 32); c.r[d] = uint32_t(s); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_addme(Ctx& c, int d, int a, bool rc) {
    uint64_t s = uint64_t(c.r[a]) + 0xFFFFFFFFull + c.xer_ca; c.xer_ca = uint8_t(s >> 32); c.r[d] = uint32_t(s); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_subf(Ctx& c, int d, int a, int b, bool rc) { c.r[d] = c.r[b] - c.r[a]; rc_set(c, c.r[d], rc); }
PPC_INLINE void op_subfc(Ctx& c, int d, int a, int b, bool rc) {
    uint32_t x = c.r[a], y = c.r[b]; c.xer_ca = y >= x; c.r[d] = y - x; rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_subfe(Ctx& c, int d, int a, int b, bool rc) {
    uint64_t s = uint64_t(~c.r[a]) + c.r[b] + c.xer_ca; c.xer_ca = uint8_t(s >> 32); c.r[d] = uint32_t(s); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_subfze(Ctx& c, int d, int a, bool rc) {
    uint64_t s = uint64_t(~c.r[a]) + c.xer_ca; c.xer_ca = uint8_t(s >> 32); c.r[d] = uint32_t(s); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_subfme(Ctx& c, int d, int a, bool rc) {
    uint64_t s = uint64_t(~c.r[a]) + 0xFFFFFFFFull + c.xer_ca; c.xer_ca = uint8_t(s >> 32); c.r[d] = uint32_t(s); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_neg(Ctx& c, int d, int a, bool rc) { c.r[d] = 0u - c.r[a]; rc_set(c, c.r[d], rc); }
// Multiply as unsigned: the low 32 bits are identical and signed overflow would be undefined behaviour in C++.
PPC_INLINE void op_mulli(Ctx& c, int d, int a, int32_t i) { c.r[d] = c.r[a] * uint32_t(i); }
PPC_INLINE void op_mullw(Ctx& c, int d, int a, int b, bool rc) { c.r[d] = c.r[a] * c.r[b]; rc_set(c, c.r[d], rc); }
PPC_INLINE void op_mulhw(Ctx& c, int d, int a, int b, bool rc) {
    c.r[d] = uint32_t((int64_t(int32_t(c.r[a])) * int32_t(c.r[b])) >> 32); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_mulhwu(Ctx& c, int d, int a, int b, bool rc) {
    c.r[d] = uint32_t((uint64_t(c.r[a]) * c.r[b]) >> 32); rc_set(c, c.r[d], rc);
}
// Division by zero / INT_MIN/-1 is undefined in the ISA; use the hardware-observed result (0, or all ones for a negative dividend).
PPC_INLINE void op_divw(Ctx& c, int d, int a, int b, bool rc) {
    int32_t x = int32_t(c.r[a]), y = int32_t(c.r[b]);
    c.r[d] = (y == 0 || (x == INT32_MIN && y == -1)) ? (x < 0 ? 0xFFFFFFFFu : 0u) : uint32_t(x / y);
    rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_divwu(Ctx& c, int d, int a, int b, bool rc) { c.r[d] = c.r[b] ? c.r[a] / c.r[b] : 0u; rc_set(c, c.r[d], rc); }

// ---- logical, shifts, rotates (t = rS, a = rA destination) ----
PPC_INLINE void op_and(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = c.r[s] & c.r[b]; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_andc(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = c.r[s] & ~c.r[b]; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_or(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = c.r[s] | c.r[b]; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_orc(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = c.r[s] | ~c.r[b]; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_xor(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = c.r[s] ^ c.r[b]; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_nor(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = ~(c.r[s] | c.r[b]); rc_set(c, c.r[a], rc); }
PPC_INLINE void op_nand(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = ~(c.r[s] & c.r[b]); rc_set(c, c.r[a], rc); }
PPC_INLINE void op_eqv(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = ~(c.r[s] ^ c.r[b]); rc_set(c, c.r[a], rc); }
PPC_INLINE void op_ori(Ctx& c, int s, int a, uint32_t i) { c.r[a] = c.r[s] | i; }
PPC_INLINE void op_oris(Ctx& c, int s, int a, uint32_t i) { c.r[a] = c.r[s] | (i << 16); }
PPC_INLINE void op_xori(Ctx& c, int s, int a, uint32_t i) { c.r[a] = c.r[s] ^ i; }
PPC_INLINE void op_xoris(Ctx& c, int s, int a, uint32_t i) { c.r[a] = c.r[s] ^ (i << 16); }
PPC_INLINE void op_andi_(Ctx& c, int s, int a, uint32_t i) { c.r[a] = c.r[s] & i; cr0(c, c.r[a]); }
PPC_INLINE void op_andis_(Ctx& c, int s, int a, uint32_t i) { c.r[a] = c.r[s] & (i << 16); cr0(c, c.r[a]); }
PPC_INLINE void op_extsb(Ctx& c, int s, int a, bool rc) { c.r[a] = uint32_t(int32_t(int8_t(c.r[s]))); rc_set(c, c.r[a], rc); }
PPC_INLINE void op_extsh(Ctx& c, int s, int a, bool rc) { c.r[a] = uint32_t(int32_t(int16_t(c.r[s]))); rc_set(c, c.r[a], rc); }
PPC_INLINE void op_cntlzw(Ctx& c, int s, int a, bool rc) { uint32_t v = c.r[s]; c.r[a] = v ? uint32_t(__builtin_clz(v)) : 32u; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_slw(Ctx& c, int s, int a, int b, bool rc) { uint32_t n = c.r[b] & 0x3F; c.r[a] = n & 32 ? 0u : c.r[s] << n; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_srw(Ctx& c, int s, int a, int b, bool rc) { uint32_t n = c.r[b] & 0x3F; c.r[a] = n & 32 ? 0u : c.r[s] >> n; rc_set(c, c.r[a], rc); }
PPC_INLINE void sra_common(Ctx& c, int s, int a, uint32_t n, bool rc) {
    int32_t v = int32_t(c.r[s]);
    if (n >= 32) { c.r[a] = v < 0 ? 0xFFFFFFFFu : 0u; c.xer_ca = v < 0; }
    else { c.r[a] = uint32_t(v >> n); c.xer_ca = v < 0 && n && (uint32_t(v) & ((1u << n) - 1)) != 0; }
    rc_set(c, c.r[a], rc);
}
PPC_INLINE void op_sraw(Ctx& c, int s, int a, int b, bool rc) { sra_common(c, s, a, c.r[b] & 0x3F, rc); }
PPC_INLINE void op_srawi(Ctx& c, int s, int a, int sh, bool rc) { sra_common(c, s, a, uint32_t(sh), rc); }
PPC_INLINE uint32_t rotl32(uint32_t v, uint32_t n) { return _rotl(v, int(n & 31)); }
PPC_INLINE uint32_t mask32(uint32_t mb, uint32_t me) {
    uint32_t m = (0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me));
    return mb <= me ? m : ((0xFFFFFFFFu >> mb) | (0xFFFFFFFFu << (31 - me)));
}
PPC_INLINE void op_rlwinm(Ctx& c, int s, int a, int sh, int mb, int me, bool rc) {
    c.r[a] = rotl32(c.r[s], sh) & mask32(mb, me); rc_set(c, c.r[a], rc);
}
PPC_INLINE void op_rlwnm(Ctx& c, int s, int a, int b, int mb, int me, bool rc) {
    c.r[a] = rotl32(c.r[s], c.r[b]) & mask32(mb, me); rc_set(c, c.r[a], rc);
}
PPC_INLINE void op_rlwimi(Ctx& c, int s, int a, int sh, int mb, int me, bool rc) {
    uint32_t m = mask32(mb, me); c.r[a] = (rotl32(c.r[s], sh) & m) | (c.r[a] & ~m); rc_set(c, c.r[a], rc);
}

// ---- compares ----
PPC_INLINE void cmp_set(Ctx& c, int f, int cmp) { c.cr[f] = uint8_t((cmp < 0 ? 8 : cmp > 0 ? 4 : 2) | c.xer_so); }
PPC_INLINE int cmp3(int64_t x, int64_t y) { return x < y ? -1 : x > y ? 1 : 0; }
PPC_INLINE void op_cmp(Ctx& c, int f, int a, int b) { cmp_set(c, f, cmp3(int32_t(c.r[a]), int32_t(c.r[b]))); }
PPC_INLINE void op_cmpl(Ctx& c, int f, int a, int b) { cmp_set(c, f, cmp3(c.r[a], c.r[b])); }
PPC_INLINE void op_cmpi(Ctx& c, int f, int a, int32_t i) { cmp_set(c, f, cmp3(int32_t(c.r[a]), i)); }
PPC_INLINE void op_cmpli(Ctx& c, int f, int a, uint32_t i) { cmp_set(c, f, cmp3(c.r[a], i)); }

// ---- special registers ----
PPC_INLINE uint32_t xer_get(Ctx& c) { return uint32_t(c.xer_so) << 31 | uint32_t(c.xer_ov) << 30 | uint32_t(c.xer_ca) << 29; }
PPC_INLINE void op_mfspr(Ctx& c, int t, uint32_t s) {
    switch (s) {
        case 1: c.r[t] = xer_get(c); break;
        case 8: c.r[t] = c.lr; break;
        case 9: c.r[t] = c.ctr; break;
        case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919: c.r[t] = c.gqr[s - 912]; break;
        default: c.r[t] = c.spr[s & 1023]; break;
    }
}
PPC_INLINE void op_mtspr(Ctx& c, int t, uint32_t s) {
    uint32_t v = c.r[t];
    switch (s) {
        case 1: c.xer_so = uint8_t(v >> 31); c.xer_ov = (v >> 30) & 1; c.xer_ca = (v >> 29) & 1; break;
        case 8: c.lr = v; break;
        case 9: c.ctr = v; break;
        case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919: c.gqr[s - 912] = v; break;
        default: c.spr[s & 1023] = v; break;
    }
}
PPC_INLINE void op_mftb(Ctx& c, int t, uint32_t tbr) { uint64_t tb = timebase(); c.r[t] = tbr == 269 ? uint32_t(tb >> 32) : uint32_t(tb); }
PPC_INLINE void op_mfmsr(Ctx& c, int t) { c.r[t] = c.msr; }
PPC_INLINE void op_mtmsr(Ctx& c, int t) { c.msr = c.r[t]; }
PPC_INLINE void op_isync(Ctx&) {}
PPC_INLINE void op_sync(Ctx&) { _ReadWriteBarrier(); }
PPC_INLINE void op_eieio(Ctx&) { _ReadWriteBarrier(); }
PPC_INLINE void op_dcbf(Ctx&, int, int) {}
PPC_INLINE void op_dcbst(Ctx&, int, int) {}
PPC_INLINE void op_dcbi(Ctx&, int, int) {}
PPC_INLINE void op_dcbt(Ctx&, int, int) {}
PPC_INLINE void op_dcbtst(Ctx&, int, int) {}
PPC_INLINE void op_icbi(Ctx&, int, int) {}
PPC_INLINE void op_tw(Ctx& c, int, int, int) { (void)c; }   // unconditional traps are emitted by the translator
PPC_INLINE void op_twi(Ctx& c, int, int, int32_t) { (void)c; }

// ---- loads and stores ----
#define PPC_ID(x) (x)
#define PPC_SX16(x) (uint32_t(int32_t(int16_t(x))))
#define PPC_LOAD_FAMILY(nm, RD, CV) \
    PPC_INLINE void op_##nm(Ctx& c, int t, int a, int32_t d) { c.r[t] = CV(RD(c, ea_d(c, a, d))); } \
    PPC_INLINE void op_##nm##u(Ctx& c, int t, int a, int32_t d) { uint32_t e = c.r[a] + uint32_t(d); c.r[t] = CV(RD(c, e)); c.r[a] = e; } \
    PPC_INLINE void op_##nm##x(Ctx& c, int t, int a, int b) { c.r[t] = CV(RD(c, ea_x(c, a, b))); } \
    PPC_INLINE void op_##nm##ux(Ctx& c, int t, int a, int b) { uint32_t e = c.r[a] + c.r[b]; c.r[t] = CV(RD(c, e)); c.r[a] = e; }
PPC_LOAD_FAMILY(lwz, rd32, PPC_ID)
PPC_LOAD_FAMILY(lbz, rd8, PPC_ID)
PPC_LOAD_FAMILY(lhz, rd16, PPC_ID)
PPC_LOAD_FAMILY(lha, rd16, PPC_SX16)

#define PPC_STORE_FAMILY(nm, WR) \
    PPC_INLINE void op_##nm(Ctx& c, int t, int a, int32_t d) { WR(c, ea_d(c, a, d), c.r[t]); } \
    PPC_INLINE void op_##nm##u(Ctx& c, int t, int a, int32_t d) { uint32_t e = c.r[a] + uint32_t(d); WR(c, e, c.r[t]); c.r[a] = e; } \
    PPC_INLINE void op_##nm##x(Ctx& c, int t, int a, int b) { WR(c, ea_x(c, a, b), c.r[t]); } \
    PPC_INLINE void op_##nm##ux(Ctx& c, int t, int a, int b) { uint32_t e = c.r[a] + c.r[b]; WR(c, e, c.r[t]); c.r[a] = e; }
PPC_STORE_FAMILY(stw, wr32)
PPC_STORE_FAMILY(stb, wr8)
PPC_STORE_FAMILY(sth, wr16)

PPC_INLINE void op_lwbrx(Ctx& c, int t, int a, int b) { c.r[t] = _byteswap_ulong(rd32(c, ea_x(c, a, b))); }
PPC_INLINE void op_lhbrx(Ctx& c, int t, int a, int b) { c.r[t] = _byteswap_ushort(uint16_t(rd16(c, ea_x(c, a, b)))); }
PPC_INLINE void op_stwbrx(Ctx& c, int t, int a, int b) { wr32(c, ea_x(c, a, b), _byteswap_ulong(c.r[t])); }
PPC_INLINE void op_sthbrx(Ctx& c, int t, int a, int b) { wr16(c, ea_x(c, a, b), _byteswap_ushort(uint16_t(c.r[t]))); }
PPC_INLINE void op_lmw(Ctx& c, int t, int a, int32_t d) { uint32_t e = ea_d(c, a, d); for (int i = t; i < 32; ++i, e += 4) c.r[i] = rd32(c, e); }
PPC_INLINE void op_stmw(Ctx& c, int t, int a, int32_t d) { uint32_t e = ea_d(c, a, d); for (int i = t; i < 32; ++i, e += 4) wr32(c, e, c.r[i]); }
// Single guest-visible reservation; guest threads never run truly concurrently with the scheduler lock held.
PPC_INLINE void op_lwarx(Ctx& c, int t, int a, int b) { uint32_t e = ea_x(c, a, b); c.reserve_addr = e | 1u; c.r[t] = rd32(c, e); }
PPC_INLINE void op_stwcx_(Ctx& c, int t, int a, int b) {
    uint32_t e = ea_x(c, a, b);
    bool ok = c.reserve_addr == (e | 1u);
    if (ok) wr32(c, e, c.r[t]);
    c.reserve_addr = 0;
    c.cr[0] = uint8_t((ok ? 2 : 0) | c.xer_so);
}
PPC_INLINE void op_dcbz(Ctx& c, int a, int b) { uint32_t e = ea_x(c, a, b) & ~31u; std::memset(hp(c, e), 0, 32); }
PPC_INLINE void op_dcbz_l(Ctx& c, int a, int b) { op_dcbz(c, a, b); }

// ---- floating point ----
PPC_INLINE double rnd_s(double v) { return double(float(v)); }
PPC_INLINE void fp_rc(Ctx& c, bool rc) { if (rc) c.cr[1] = uint8_t(c.fpscr >> 28); }

PPC_INLINE void op_lfs(Ctx& c, int t, int a, int32_t d) { double v = rdf32(c, ea_d(c, a, d)); c.f[t] = v; c.ps1[t] = v; }
PPC_INLINE void op_lfsu(Ctx& c, int t, int a, int32_t d) { uint32_t e = c.r[a] + uint32_t(d); double v = rdf32(c, e); c.f[t] = v; c.ps1[t] = v; c.r[a] = e; }
PPC_INLINE void op_lfsx(Ctx& c, int t, int a, int b) { double v = rdf32(c, ea_x(c, a, b)); c.f[t] = v; c.ps1[t] = v; }
PPC_INLINE void op_lfsux(Ctx& c, int t, int a, int b) { uint32_t e = c.r[a] + c.r[b]; double v = rdf32(c, e); c.f[t] = v; c.ps1[t] = v; c.r[a] = e; }
PPC_INLINE void op_lfd(Ctx& c, int t, int a, int32_t d) { c.f[t] = rdf64(c, ea_d(c, a, d)); }
PPC_INLINE void op_lfdu(Ctx& c, int t, int a, int32_t d) { uint32_t e = c.r[a] + uint32_t(d); c.f[t] = rdf64(c, e); c.r[a] = e; }
PPC_INLINE void op_lfdx(Ctx& c, int t, int a, int b) { c.f[t] = rdf64(c, ea_x(c, a, b)); }
PPC_INLINE void op_lfdux(Ctx& c, int t, int a, int b) { uint32_t e = c.r[a] + c.r[b]; c.f[t] = rdf64(c, e); c.r[a] = e; }
PPC_INLINE void op_stfs(Ctx& c, int t, int a, int32_t d) { wrf32(c, ea_d(c, a, d), float(c.f[t])); }
PPC_INLINE void op_stfsu(Ctx& c, int t, int a, int32_t d) { uint32_t e = c.r[a] + uint32_t(d); wrf32(c, e, float(c.f[t])); c.r[a] = e; }
PPC_INLINE void op_stfsx(Ctx& c, int t, int a, int b) { wrf32(c, ea_x(c, a, b), float(c.f[t])); }
PPC_INLINE void op_stfsux(Ctx& c, int t, int a, int b) { uint32_t e = c.r[a] + c.r[b]; wrf32(c, e, float(c.f[t])); c.r[a] = e; }
PPC_INLINE void op_stfd(Ctx& c, int t, int a, int32_t d) { wrf64(c, ea_d(c, a, d), c.f[t]); }
PPC_INLINE void op_stfdu(Ctx& c, int t, int a, int32_t d) { uint32_t e = c.r[a] + uint32_t(d); wrf64(c, e, c.f[t]); c.r[a] = e; }
PPC_INLINE void op_stfdx(Ctx& c, int t, int a, int b) { wrf64(c, ea_x(c, a, b), c.f[t]); }
PPC_INLINE void op_stfdux(Ctx& c, int t, int a, int b) { uint32_t e = c.r[a] + c.r[b]; wrf64(c, e, c.f[t]); c.r[a] = e; }
PPC_INLINE void op_stfiwx(Ctx& c, int t, int a, int b) { uint64_t bt; std::memcpy(&bt, &c.f[t], 8); wr32(c, ea_x(c, a, b), uint32_t(bt)); }

// Single-precision results are written to both halves of the paired-single register (Gekko); double-precision ones only to ps0.
#define PPC_SYNC_PPC_ID(c, t)
#define PPC_SYNC_rnd_s(c, t) (c).ps1[t] = (c).f[t]
#define PPC_FP_BIN(nm, EXPR, RND) \
    PPC_INLINE void op_##nm(Ctx& c, int t, int a, int b, bool rc) { double x = c.f[a], y = c.f[b]; c.f[t] = RND(EXPR); PPC_SYNC_##RND(c, t); fp_rc(c, rc); }
PPC_FP_BIN(fadd, x + y, PPC_ID)   PPC_FP_BIN(fsub, x - y, PPC_ID)   PPC_FP_BIN(fdiv, x / y, PPC_ID)
PPC_FP_BIN(fadds, x + y, rnd_s)   PPC_FP_BIN(fsubs, x - y, rnd_s)   PPC_FP_BIN(fdivs, x / y, rnd_s)
PPC_INLINE void op_fmul(Ctx& c, int t, int a, int cc, bool rc) { c.f[t] = c.f[a] * c.f[cc]; fp_rc(c, rc); }
PPC_INLINE void op_fmuls(Ctx& c, int t, int a, int cc, bool rc) { c.f[t] = rnd_s(c.f[a] * c.f[cc]); c.ps1[t] = c.f[t]; fp_rc(c, rc); }
#define PPC_FP_FMA(nm, EXPR, RND) \
    PPC_INLINE void op_##nm(Ctx& c, int t, int a, int b, int cc, bool rc) { double x = c.f[a], y = c.f[b], z = c.f[cc]; c.f[t] = RND(EXPR); PPC_SYNC_##RND(c, t); fp_rc(c, rc); }
PPC_FP_FMA(fmadd, std::fma(x, z, y), PPC_ID)    PPC_FP_FMA(fmsub, std::fma(x, z, -y), PPC_ID)
PPC_FP_FMA(fnmadd, -std::fma(x, z, y), PPC_ID)  PPC_FP_FMA(fnmsub, -std::fma(x, z, -y), PPC_ID)
PPC_FP_FMA(fmadds, std::fma(x, z, y), rnd_s)    PPC_FP_FMA(fmsubs, std::fma(x, z, -y), rnd_s)
PPC_FP_FMA(fnmadds, -std::fma(x, z, y), rnd_s)  PPC_FP_FMA(fnmsubs, -std::fma(x, z, -y), rnd_s)
PPC_INLINE void op_fsel(Ctx& c, int t, int a, int b, int cc, bool rc) { c.f[t] = c.f[a] >= 0.0 ? c.f[cc] : c.f[b]; fp_rc(c, rc); }
PPC_INLINE void op_fres(Ctx& c, int t, int b, bool rc) { c.f[t] = rnd_s(1.0 / c.f[b]); c.ps1[t] = c.f[t]; fp_rc(c, rc); }
PPC_INLINE void op_frsqrte(Ctx& c, int t, int b, bool rc) { c.f[t] = 1.0 / std::sqrt(c.f[b]); fp_rc(c, rc); }
PPC_INLINE void op_fsqrts(Ctx& c, int t, int b, bool rc) { c.f[t] = rnd_s(std::sqrt(c.f[b])); c.ps1[t] = c.f[t]; fp_rc(c, rc); }
PPC_INLINE void op_frsp(Ctx& c, int t, int b, bool rc) { c.f[t] = rnd_s(c.f[b]); c.ps1[t] = c.f[t]; fp_rc(c, rc); }
PPC_INLINE void fcti(Ctx& c, int t, double v, bool trunc, bool rc) {
    double r = trunc ? std::trunc(v) : std::nearbyint(v);   // default rounding mode is to-nearest-even
    int32_t i = std::isnan(v) || r <= -2147483648.0 ? INT32_MIN : r >= 2147483647.0 ? INT32_MAX : int32_t(r);
    uint64_t bt = 0xFFF8000000000000ull | uint32_t(i); std::memcpy(&c.f[t], &bt, 8); fp_rc(c, rc);
}
PPC_INLINE void op_fctiw(Ctx& c, int t, int b, bool rc) { fcti(c, t, c.f[b], false, rc); }
PPC_INLINE void op_fctiwz(Ctx& c, int t, int b, bool rc) { fcti(c, t, c.f[b], true, rc); }
PPC_INLINE void fcmp(Ctx& c, int f, double x, double y) {
    c.cr[f] = std::isnan(x) || std::isnan(y) ? 1 : x < y ? 8 : x > y ? 4 : 2;
}
PPC_INLINE void op_fcmpu(Ctx& c, int f, int a, int b) { fcmp(c, f, c.f[a], c.f[b]); }
PPC_INLINE void op_fcmpo(Ctx& c, int f, int a, int b) { fcmp(c, f, c.f[a], c.f[b]); }
PPC_INLINE void op_fmr(Ctx& c, int t, int b, bool rc) { c.f[t] = c.f[b]; fp_rc(c, rc); }
PPC_INLINE void op_fneg(Ctx& c, int t, int b, bool rc) { c.f[t] = -c.f[b]; fp_rc(c, rc); }
PPC_INLINE void op_fabs(Ctx& c, int t, int b, bool rc) { c.f[t] = std::fabs(c.f[b]); fp_rc(c, rc); }
PPC_INLINE void op_fnabs(Ctx& c, int t, int b, bool rc) { c.f[t] = -std::fabs(c.f[b]); fp_rc(c, rc); }
PPC_INLINE void op_mffs(Ctx& c, int t, bool rc) { uint64_t bt = 0xFFF8000000000000ull | c.fpscr; std::memcpy(&c.f[t], &bt, 8); fp_rc(c, rc); }
PPC_INLINE void op_mtfsf(Ctx& c, int fm, int b, bool rc) {
    uint64_t bt; std::memcpy(&bt, &c.f[b], 8);
    for (int i = 0; i < 8; ++i) if (fm & (0x80 >> i)) c.fpscr = (c.fpscr & ~(0xFu << (28 - 4 * i))) | (uint32_t(bt) & (0xFu << (28 - 4 * i)));
    fp_rc(c, rc);
}
PPC_INLINE void op_mtfsfi(Ctx& c, int crfd, int imm, bool rc) { c.fpscr = (c.fpscr & ~(0xFu << (28 - 4 * crfd))) | (uint32_t(imm & 0xF) << (28 - 4 * crfd)); fp_rc(c, rc); }
PPC_INLINE void op_mtfsb0(Ctx& c, int bit, bool rc) { c.fpscr &= ~(0x80000000u >> bit); fp_rc(c, rc); }
PPC_INLINE void op_mtfsb1(Ctx& c, int bit, bool rc) { c.fpscr |= 0x80000000u >> bit; fp_rc(c, rc); }
PPC_INLINE void op_mcrfs(Ctx& c, int d, int s) { c.cr[d] = uint8_t((c.fpscr >> (28 - 4 * s)) & 0xF); c.fpscr &= ~(0xFu << (28 - 4 * s)); }

// ---- paired singles ----
#define PPC_PS_BIN(nm, EXPR) \
    PPC_INLINE void op_##nm(Ctx& c, int t, int a, int b, bool rc) { \
        double x0 = c.f[a], x1 = c.ps1[a], y0 = c.f[b], y1 = c.ps1[b]; \
        c.f[t] = rnd_s(EXPR(x0, y0)); c.ps1[t] = rnd_s(EXPR(x1, y1)); fp_rc(c, rc); }
#define PS_ADD(x, y) ((x) + (y))
#define PS_SUB(x, y) ((x) - (y))
#define PS_DIV(x, y) ((x) / (y))
PPC_PS_BIN(ps_add, PS_ADD) PPC_PS_BIN(ps_sub, PS_SUB) PPC_PS_BIN(ps_div, PS_DIV)
PPC_INLINE void op_ps_mul(Ctx& c, int t, int a, int cc, bool rc) { double p0 = rnd_s(c.f[a] * c.f[cc]), p1 = rnd_s(c.ps1[a] * c.ps1[cc]); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_muls0(Ctx& c, int t, int a, int cc, bool rc) { double s = c.f[cc], p0 = rnd_s(c.f[a] * s), p1 = rnd_s(c.ps1[a] * s); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_muls1(Ctx& c, int t, int a, int cc, bool rc) { double s = c.ps1[cc], p0 = rnd_s(c.f[a] * s), p1 = rnd_s(c.ps1[a] * s); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
#define PPC_PS_FMA(nm, E0, E1) \
    PPC_INLINE void op_##nm(Ctx& c, int t, int a, int b, int cc, bool rc) { \
        double a0 = c.f[a], a1 = c.ps1[a], b0 = c.f[b], b1 = c.ps1[b], c0 = c.f[cc], c1 = c.ps1[cc]; \
        double r0 = rnd_s(E0), r1 = rnd_s(E1); c.f[t] = r0; c.ps1[t] = r1; fp_rc(c, rc); }
PPC_PS_FMA(ps_madd, std::fma(a0, c0, b0), std::fma(a1, c1, b1))
PPC_PS_FMA(ps_msub, std::fma(a0, c0, -b0), std::fma(a1, c1, -b1))
PPC_PS_FMA(ps_nmadd, -std::fma(a0, c0, b0), -std::fma(a1, c1, b1))
PPC_PS_FMA(ps_nmsub, -std::fma(a0, c0, -b0), -std::fma(a1, c1, -b1))
PPC_PS_FMA(ps_madds0, std::fma(a0, c0, b0), std::fma(a1, c0, b1))
PPC_PS_FMA(ps_madds1, std::fma(a0, c1, b0), std::fma(a1, c1, b1))
PPC_PS_FMA(ps_sel, (a0 >= 0.0 ? c0 : b0), (a1 >= 0.0 ? c1 : b1))
PPC_PS_FMA(ps_sum0, (a0 + b1), c1)
PPC_PS_FMA(ps_sum1, c0, (a0 + b1))
PPC_INLINE void op_ps_res(Ctx& c, int t, int b, bool rc) { double p0 = rnd_s(1.0 / c.f[b]), p1 = rnd_s(1.0 / c.ps1[b]); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_rsqrte(Ctx& c, int t, int b, bool rc) { double p0 = rnd_s(1.0 / std::sqrt(c.f[b])), p1 = rnd_s(1.0 / std::sqrt(c.ps1[b])); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_neg(Ctx& c, int t, int b, bool rc) { double p0 = -c.f[b], p1 = -c.ps1[b]; c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_abs(Ctx& c, int t, int b, bool rc) { double p0 = std::fabs(c.f[b]), p1 = std::fabs(c.ps1[b]); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_nabs(Ctx& c, int t, int b, bool rc) { double p0 = -std::fabs(c.f[b]), p1 = -std::fabs(c.ps1[b]); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_mr(Ctx& c, int t, int b, bool rc) { double p0 = c.f[b], p1 = c.ps1[b]; c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_merge00(Ctx& c, int t, int a, int b, bool rc) { double p0 = c.f[a], p1 = c.f[b]; c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_merge01(Ctx& c, int t, int a, int b, bool rc) { double p0 = c.f[a], p1 = c.ps1[b]; c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_merge10(Ctx& c, int t, int a, int b, bool rc) { double p0 = c.ps1[a], p1 = c.f[b]; c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_merge11(Ctx& c, int t, int a, int b, bool rc) { double p0 = c.ps1[a], p1 = c.ps1[b]; c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_cmpu0(Ctx& c, int f, int a, int b) { fcmp(c, f, c.f[a], c.f[b]); }
PPC_INLINE void op_ps_cmpo0(Ctx& c, int f, int a, int b) { fcmp(c, f, c.f[a], c.f[b]); }
PPC_INLINE void op_ps_cmpu1(Ctx& c, int f, int a, int b) { fcmp(c, f, c.ps1[a], c.ps1[b]); }
PPC_INLINE void op_ps_cmpo1(Ctx& c, int f, int a, int b) { fcmp(c, f, c.ps1[a], c.ps1[b]); }

// ---- quantized load/store (GQR: LD_SCALE bits 24-29, LD_TYPE 16-18, ST_SCALE 8-13, ST_TYPE 0-2; types 4=u8 5=u16 6=s8 7=s16, else float) ----
PPC_INLINE int gq_scale(uint32_t v) { int s = int(v & 0x3F); return s & 0x20 ? s - 64 : s; }
PPC_INLINE int gq_bytes(int type) { return (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4; }
PPC_INLINE double gq_load1(Ctx& c, uint32_t ea, int type, int scale) {
    double v;
    switch (type) {
        case 4: v = double(rd8(c, ea)); break;
        case 5: v = double(rd16(c, ea)); break;
        case 6: v = double(int8_t(rd8(c, ea))); break;
        case 7: v = double(int16_t(rd16(c, ea))); break;
        default: return double(rdf32(c, ea));
    }
    return v * std::ldexp(1.0, -scale);
}
PPC_INLINE void gq_store1(Ctx& c, uint32_t ea, int type, int scale, double v) {
    if (type < 4 || type > 7) { wrf32(c, ea, float(v)); return; }
    double x = std::trunc(v * std::ldexp(1.0, scale));
    double lo = type == 4 ? 0 : type == 5 ? 0 : type == 6 ? -128 : -32768;
    double hi = type == 4 ? 255 : type == 5 ? 65535 : type == 6 ? 127 : 32767;
    x = std::isnan(x) ? lo : x < lo ? lo : x > hi ? hi : x;
    if (gq_bytes(type) == 1) wr8(c, ea, uint32_t(int32_t(x))); else wr16(c, ea, uint32_t(int32_t(x)));
}
PPC_INLINE void psq_load(Ctx& c, int t, uint32_t ea, int w, int i) {
    uint32_t g = c.gqr[i]; int type = (g >> 16) & 7, scale = gq_scale(g >> 24);
    double p0 = gq_load1(c, ea, type, scale);
    double p1 = w ? 1.0 : gq_load1(c, ea + gq_bytes(type), type, scale);
    c.f[t] = p0; c.ps1[t] = p1;
}
PPC_INLINE void psq_store(Ctx& c, int t, uint32_t ea, int w, int i) {
    uint32_t g = c.gqr[i]; int type = g & 7, scale = gq_scale(g >> 8);
    gq_store1(c, ea, type, scale, c.f[t]);
    if (!w) gq_store1(c, ea + gq_bytes(type), type, scale, c.ps1[t]);
}
PPC_INLINE void op_psq_l(Ctx& c, int t, int a, int32_t d, int w, int i) { psq_load(c, t, ea_d(c, a, d), w, i); }
PPC_INLINE void op_psq_lu(Ctx& c, int t, int a, int32_t d, int w, int i) { uint32_t e = c.r[a] + uint32_t(d); psq_load(c, t, e, w, i); c.r[a] = e; }
PPC_INLINE void op_psq_st(Ctx& c, int t, int a, int32_t d, int w, int i) { psq_store(c, t, ea_d(c, a, d), w, i); }
PPC_INLINE void op_psq_stu(Ctx& c, int t, int a, int32_t d, int w, int i) { uint32_t e = c.r[a] + uint32_t(d); psq_store(c, t, e, w, i); c.r[a] = e; }
PPC_INLINE void op_psq_lx(Ctx& c, int t, int a, int b, int w, int i) { psq_load(c, t, ea_x(c, a, b), w, i); }
PPC_INLINE void op_psq_lux(Ctx& c, int t, int a, int b, int w, int i) { uint32_t e = c.r[a] + c.r[b]; psq_load(c, t, e, w, i); c.r[a] = e; }
PPC_INLINE void op_psq_stx(Ctx& c, int t, int a, int b, int w, int i) { psq_store(c, t, ea_x(c, a, b), w, i); }
PPC_INLINE void op_psq_stux(Ctx& c, int t, int a, int b, int w, int i) { uint32_t e = c.r[a] + c.r[b]; psq_store(c, t, e, w, i); c.r[a] = e; }

}  // namespace ppc
