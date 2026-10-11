#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace enc {
inline uint32_t u(int v) { return uint32_t(v); }
inline uint32_t D(uint32_t op, int t, int a, int imm) { return op << 26 | u(t) << 21 | u(a) << 16 | (u(imm) & 0xFFFF); }
inline uint32_t X(uint32_t op, int t, int a, int b, uint32_t xo, uint32_t rc = 0) { return op << 26 | u(t) << 21 | u(a) << 16 | u(b) << 11 | xo << 1 | rc; }
inline uint32_t addi(int t, int a, int i) { return D(14, t, a, i); }
inline uint32_t addis(int t, int a, int i) { return D(15, t, a, i); }
inline uint32_t li(int t, int i) { return addi(t, 0, i); }
inline uint32_t lis(int t, int i) { return addis(t, 0, i); }
inline uint32_t ori(int a, int s, int k) { return D(24, s, a, k); }
inline uint32_t cmplwi(int a, int k) { return D(10, 0, a, k); }
inline uint32_t cmpwi(int a, int i) { return D(11, 0, a, i); }
inline uint32_t lwz(int t, int d, int a) { return D(32, t, a, d); }
inline uint32_t stw(int s, int d, int a) { return D(36, s, a, d); }
inline uint32_t stwu(int s, int d, int a) { return D(37, s, a, d); }
inline uint32_t lfs(int t, int d, int a) { return D(48, t, a, d); }
inline uint32_t stfs(int s, int d, int a) { return D(52, s, a, d); }
inline uint32_t add(int t, int a, int b) { return X(31, t, a, b, 266); }
inline uint32_t lwzx(int t, int a, int b) { return X(31, t, a, b, 23); }
inline uint32_t mr(int a, int s) { return X(31, s, a, s, 444); }
inline uint32_t mtctr(int r) { return 0x7C0903A6u | u(r) << 21; }
inline uint32_t mflr(int r) { return 0x7C0802A6u | u(r) << 21; }
inline uint32_t mtlr(int r) { return 0x7C0803A6u | u(r) << 21; }
inline uint32_t slwi(int a, int s, int n) { return 21u << 26 | u(s) << 21 | u(a) << 16 | u(n) << 11 | u(31 - n) << 1; }
inline uint32_t b(int32_t off) { return 18u << 26 | (uint32_t(off) & 0x03FFFFFC); }
inline uint32_t bl(int32_t off) { return b(off) | 1; }
inline uint32_t bc(int bo, int bi, int32_t off) { return 16u << 26 | u(bo) << 21 | u(bi) << 16 | (uint32_t(off) & 0xFFFC); }
inline uint32_t bdnz(int32_t off) { return bc(16, 0, off); }
inline uint32_t bgt(int32_t off) { return bc(12, 1, off); }
inline uint32_t blt(int32_t off) { return bc(12, 0, off); }
constexpr uint32_t blr = 0x4E800020u, bctr = 0x4E800420u, bctrl = 0x4E800421u;
inline uint32_t fadds(int t, int a, int b_) { return 59u << 26 | u(t) << 21 | u(a) << 16 | u(b_) << 11 | 21u << 1; }
inline uint32_t fctiwz(int t, int b_) { return 63u << 26 | u(t) << 21 | u(b_) << 11 | 15u << 1; }
inline uint32_t stfiwx(int s, int a, int b_) { return X(31, s, a, b_, 983); }
inline uint32_t ps_add(int t, int a, int b_) { return 4u << 26 | u(t) << 21 | u(a) << 16 | u(b_) << 11 | 21u << 1; }
inline uint32_t psq_l(int t, int d, int a, int w, int i) { return 56u << 26 | u(t) << 21 | u(a) << 16 | u(w) << 15 | u(i) << 12 | (u(d) & 0xFFF); }
inline uint32_t psq_st(int s, int d, int a, int w, int i) { return 60u << 26 | u(s) << 21 | u(a) << 16 | u(w) << 15 | u(i) << 12 | (u(d) & 0xFFF); }

struct Asm {
    uint32_t base;
    std::vector<uint32_t> w;
    explicit Asm(uint32_t base_ = 0x80003100) : base(base_) {}
    uint32_t here() const { return base + 4 * uint32_t(w.size()); }
    size_t emit(uint32_t x) { w.push_back(x); return w.size() - 1; }
    void b_to(uint32_t t) { emit(b(int32_t(t - here()))); }
    void bl_to(uint32_t t) { emit(bl(int32_t(t - here()))); }
    void bc_to(int bo, int bi, uint32_t t) { emit(bc(bo, bi, int32_t(t - here()))); }
    uint32_t addr(size_t idx) const { return base + 4 * uint32_t(idx); }
    void patch_b(size_t i, uint32_t t) { w[i] = b(int32_t(t - addr(i))); }
    void patch_bl(size_t i, uint32_t t) { w[i] = bl(int32_t(t - addr(i))); }
    void patch_bc(size_t i, int bo, int bi, uint32_t t) { w[i] = bc(bo, bi, int32_t(t - addr(i))); }
};
inline void be32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
}  // namespace enc
