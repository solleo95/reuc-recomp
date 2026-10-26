// The native _savegpr_N / _restgpr_N / _savefpr_N / _restfpr_N must leave guest memory, registers and the write watch exactly as the translated chain of
// one-instruction functions does. The reference runs the instruction of each chain entry in order (register numbers and offsets as translated from main.dol).
#include "check.h"
#include "ppc_fixture.h"
#include "gx_write_watch.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace ppc;

#define DECLARE(N) void hle_SaveGpr##N(Ctx&); void hle_RestGpr##N(Ctx&); void hle_SaveFpr##N(Ctx&); void hle_RestFpr##N(Ctx&);
DECLARE(14) DECLARE(15) DECLARE(16) DECLARE(17) DECLARE(18) DECLARE(19) DECLARE(20) DECLARE(21) DECLARE(22) DECLARE(23) DECLARE(24) DECLARE(25) DECLARE(26) DECLARE(27) DECLARE(28) DECLARE(29) DECLARE(30) DECLARE(31)
namespace {
using Entry = void (*)(Ctx&);
const Entry kSaveGpr[18] = {hle_SaveGpr14,hle_SaveGpr15,hle_SaveGpr16,hle_SaveGpr17,hle_SaveGpr18,hle_SaveGpr19,hle_SaveGpr20,hle_SaveGpr21,hle_SaveGpr22,hle_SaveGpr23,hle_SaveGpr24,hle_SaveGpr25,hle_SaveGpr26,hle_SaveGpr27,hle_SaveGpr28,hle_SaveGpr29,hle_SaveGpr30,hle_SaveGpr31};
const Entry kRestGpr[18] = {hle_RestGpr14,hle_RestGpr15,hle_RestGpr16,hle_RestGpr17,hle_RestGpr18,hle_RestGpr19,hle_RestGpr20,hle_RestGpr21,hle_RestGpr22,hle_RestGpr23,hle_RestGpr24,hle_RestGpr25,hle_RestGpr26,hle_RestGpr27,hle_RestGpr28,hle_RestGpr29,hle_RestGpr30,hle_RestGpr31};
const Entry kSaveFpr[18] = {hle_SaveFpr14,hle_SaveFpr15,hle_SaveFpr16,hle_SaveFpr17,hle_SaveFpr18,hle_SaveFpr19,hle_SaveFpr20,hle_SaveFpr21,hle_SaveFpr22,hle_SaveFpr23,hle_SaveFpr24,hle_SaveFpr25,hle_SaveFpr26,hle_SaveFpr27,hle_SaveFpr28,hle_SaveFpr29,hle_SaveFpr30,hle_SaveFpr31};
const Entry kRestFpr[18] = {hle_RestFpr14,hle_RestFpr15,hle_RestFpr16,hle_RestFpr17,hle_RestFpr18,hle_RestFpr19,hle_RestFpr20,hle_RestFpr21,hle_RestFpr22,hle_RestFpr23,hle_RestFpr24,hle_RestFpr25,hle_RestFpr26,hle_RestFpr27,hle_RestFpr28,hle_RestFpr29,hle_RestFpr30,hle_RestFpr31};

// The chain from register `first`: what the translated functions do one after the other (a literal list per family, independent of the HLE templates).
void ref_chain(Ctx& c, int family, int first) {
    for (int n = first; n <= 31; ++n) switch (family) {
        case 0: op_stw(c, n, 11, -4 * (32 - n)); break;
        case 1: op_lwz(c, n, 11, -4 * (32 - n)); break;
        case 2: op_stfd(c, n, 11, -8 * (32 - n)); break;
        default: op_lfd(c, n, 11, -8 * (32 - n)); break;
    }
}
constexpr uint32_t kBase = 0x80100000, kSp = kBase + 0x800;
uint64_t bits(double v) { uint64_t b; std::memcpy(&b, &v, 8); return b; }
}  // namespace

int main() {
    std::mt19937_64 rng(2026);
    int mismatches = 0;
    for (int family = 0; family < 4; ++family) for (int first = 14; first <= 31; ++first) for (int trial = 0; trial < 20; ++trial) {
        Fix a, b;
        const Entry entries[4] = {kSaveGpr[first - 14], kRestGpr[first - 14], kSaveFpr[first - 14], kRestFpr[first - 14]};
        for (Fix* f : {&a, &b}) {
            Ctx& c = f->c;
            std::mt19937_64 fill(trial * 131 + first);   // the same memory and registers for both
            for (uint32_t at = kBase; at < kBase + 0x1000; at += 4) wr32(c, at, uint32_t(fill()));
            for (int i = 0; i < 32; ++i) { c.r[i] = uint32_t(fill()); uint64_t raw = fill(); if (trial % 5 == 0) raw = 0x7FF0000000000001ull ^ (uint64_t(i) << 3); std::memcpy(&c.f[i], &raw, 8); c.ps1[i] = double(i) * 0.5; }
            c.r[11] = kSp + 4 * (trial % 4) * 8;   // varied, 4-byte aligned bases
            c.lr = 0x80012345u;
        }
        const auto stamp_a = gx::watch_stamp();
        ref_chain(a.c, family, first);
        const bool wrote_a = gx::written_since(kBase, 0x1000, stamp_a);
        const auto stamp_b = gx::watch_stamp();
        entries[family](b.c);
        const bool wrote_b = gx::written_since(kBase, 0x1000, stamp_b);
        bool same = wrote_a == wrote_b && a.c.lr == b.c.lr && std::memcmp(a.c.mem + (kBase & 0x3FFFFFFF), b.c.mem + (kBase & 0x3FFFFFFF), 0x1000) == 0;
        for (int i = 0; i < 32 && same; ++i) same = a.c.r[i] == b.c.r[i] && bits(a.c.f[i]) == bits(b.c.f[i]) && bits(a.c.ps1[i]) == bits(b.c.ps1[i]);
        if (!same && ++mismatches <= 5) std::printf("family %d from r/f%d differs (trial %d)\n", family, first, trial);
    }
    std::printf("SDK register helpers: %d mismatches in %d runs\n", mismatches, 4 * 18 * 20);
    CHECK(mismatches == 0);
    return 0;
}
