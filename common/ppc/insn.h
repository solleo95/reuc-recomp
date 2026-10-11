#pragma once
#include <cstdint>

namespace ppc {
inline uint32_t t(uint32_t w) { return (w >> 21) & 31; }
inline uint32_t a(uint32_t w) { return (w >> 16) & 31; }
inline uint32_t b(uint32_t w) { return (w >> 11) & 31; }
inline uint32_t c(uint32_t w) { return (w >> 6) & 31; }
inline int32_t simm(uint32_t w) { return int16_t(w & 0xFFFF); }
inline uint32_t uimm(uint32_t w) { return w & 0xFFFF; }
inline bool rc(uint32_t w) { return w & 1; }
inline bool oe(uint32_t w) { return (w >> 10) & 1; }
inline uint32_t mb(uint32_t w) { return (w >> 6) & 31; }
inline uint32_t me(uint32_t w) { return (w >> 1) & 31; }
inline uint32_t sh(uint32_t w) { return (w >> 11) & 31; }
inline uint32_t crfd(uint32_t w) { return (w >> 23) & 7; }
inline uint32_t crfs(uint32_t w) { return (w >> 18) & 7; }
inline uint32_t crm(uint32_t w) { return (w >> 12) & 0xFF; }
inline uint32_t spr(uint32_t w) { return (((w >> 11) & 31) << 5) | ((w >> 16) & 31); }
inline uint32_t bo(uint32_t w) { return (w >> 21) & 31; }
inline uint32_t bi(uint32_t w) { return (w >> 16) & 31; }
inline int32_t bd(uint32_t w) { return int16_t(w & 0xFFFC); }
inline int32_t li(uint32_t w) { return int32_t(w << 6) >> 6 & ~3; }
inline bool aa(uint32_t w) { return (w >> 1) & 1; }
inline bool lk(uint32_t w) { return w & 1; }
}  // namespace ppc
