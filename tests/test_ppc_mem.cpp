#include "check.h"
#include "ppc_fixture.h"
#include <algorithm>
#include <climits>
#include <random>
#include <vector>
using namespace ppc;

extern uint32_t g_mmio_last_addr, g_mmio_last_val;
extern int g_mmio_last_size;

int main() {
    Fix f; Ctx& c = f.c;
    // byte order: a big-endian word is stored most significant byte first
    wr32(c, 0x80001000, 0x11223344);
    CHECK(rd8(c, 0x80001000) == 0x11 && rd8(c, 0x80001003) == 0x44);
    CHECK(rd16(c, 0x80001000) == 0x1122 && rd16(c, 0x80001002) == 0x3344);
    // cached/uncached mirrors and MEM2 map to the same bytes
    CHECK(rd32(c, 0xC0001000) == 0x11223344);
    wr32(c, 0x90000010, 0xCAFEBABE);
    CHECK(rd32(c, 0xD0000010) == 0xCAFEBABE);
    // unaligned access works
    wr32(c, 0x80001101, 0x01020304);
    CHECK(rd32(c, 0x80001101) == 0x01020304 && rd8(c, 0x80001101) == 1);
    // MMIO window edges: 0xCC000000..0xCDFFFFFF is routed to the hook
    g_mmio_last_addr = 0;
    wr32(c, 0xCC008000, 0xAABBCCDD);
    CHECK(g_mmio_last_addr == 0xCC008000 && g_mmio_last_val == 0xAABBCCDD && g_mmio_last_size == 4);
    g_mmio_last_addr = 0; (void)rd32(c, 0xCDFFFFFC); CHECK(g_mmio_last_addr == 0xCDFFFFFC);
    CHECK(!is_mmio(0xCBFFFFFF) && is_mmio(0xCC000000) && is_mmio(0xCDFFFFFF) && !is_mmio(0xCE000000));
    // the pair test of psq_l/psq_st is the two single tests in one: around both edges of the window, at the 32-bit wrap and over a spread of addresses
    for (uint32_t base : {0xCC000000u, 0xCE000000u, 0u, 0xFFFFFFFCu}) for (int d = -12; d <= 12; ++d) {
        const uint32_t ea = base + uint32_t(d);
        CHECK(is_mmio_pair(ea) == (is_mmio(ea) || is_mmio(ea + 4)));
    }
    for (uint32_t ea = 0; ea < 0xFFFFFFF0u; ea += 0x01234567u) CHECK(is_mmio_pair(ea) == (is_mmio(ea) || is_mmio(ea + 4)));
    // locked cache window 0xE0000000..0xE0003FFF is ordinary memory for the game (dcbz_l, psq and lfs use it)
    wr32(c, 0xE0000000, 0xDEADBEEF); wr32(c, 0xE0003FFC, 0x01020304);
    CHECK(rd32(c, 0xE0000000) == 0xDEADBEEF && rd32(c, 0xE0003FFC) == 0x01020304);
    c.r[3] = 0xE0000000; c.r[4] = 0; op_dcbz_l(c, 3, 4); CHECK(rd32(c, 0xE0000000) == 0);
    // load/store instruction forms
    c.r[3] = 0x80002000; c.r[4] = 0x8;
    c.r[5] = 0xCAFE0001;
    op_stw(c, 5, 3, 0x10);          CHECK(rd32(c, 0x80002010) == 0xCAFE0001);
    op_lwz(c, 6, 3, 0x10);          CHECK(c.r[6] == 0xCAFE0001);
    op_lwzu(c, 7, 3, 0x10);         CHECK(c.r[7] == 0xCAFE0001 && c.r[3] == 0x80002010);
    op_lwzx(c, 8, 3, 4);            CHECK(c.r[8] == rd32(c, 0x80002018));
    op_lbz(c, 9, 3, 0);             CHECK(c.r[9] == 0xCA);
    wr16(c, 0x80002030, 0x8001);
    c.r[3] = 0x80002030;
    op_lha(c, 9, 3, 0);             CHECK(c.r[9] == 0xFFFF8001);
    op_lhz(c, 9, 3, 0);             CHECK(c.r[9] == 0x8001);
    // r0 base means literal zero
    wr32(c, 0x00000100, 0x5A5A5A5A);   // physical 0x100 via address 0x100 (masked)
    op_lwz(c, 9, 0, 0x100);           CHECK(c.r[9] == 0x5A5A5A5A);
    // update forms write rA, stores truncate
    c.r[3] = 0x80002100; c.r[5] = 0x1234ABCD;
    op_stbu(c, 5, 3, 4);  CHECK(rd8(c, 0x80002104) == 0xCD && c.r[3] == 0x80002104);
    op_sth(c, 5, 3, 8);   CHECK(rd16(c, 0x8000210C) == 0xABCD);
    // byte-reversed forms
    wr32(c, 0x80002200, 0x11223344); c.r[3] = 0x80002200; c.r[4] = 0;
    op_lwbrx(c, 5, 3, 4); CHECK(c.r[5] == 0x44332211);
    op_lhbrx(c, 5, 3, 4); CHECK(c.r[5] == 0x2211);
    c.r[6] = 0xAABBCCDD; op_stwbrx(c, 6, 3, 4); CHECK(rd32(c, 0x80002200) == 0xDDCCBBAA);
    // load/store multiple
    for (int i = 29; i < 32; ++i) c.r[i] = 0x100 + i;
    c.r[3] = 0x80002300;
    op_stmw(c, 29, 3, 0); CHECK(rd32(c, 0x80002300) == 0x11D && rd32(c, 0x80002308) == 0x11F);
    for (int i = 29; i < 32; ++i) c.r[i] = 0;
    op_lmw(c, 29, 3, 0);  CHECK(c.r[29] == 0x11D && c.r[31] == 0x11F);
    // reservation: stwcx. succeeds only after lwarx on the same address
    c.r[3] = 0x80002400; c.r[4] = 0; c.r[5] = 0x77;
    op_lwarx(c, 6, 3, 4);
    op_stwcx_(c, 5, 3, 4); CHECK(c.cr[0] == 2 && rd32(c, 0x80002400) == 0x77);
    op_stwcx_(c, 5, 3, 4); CHECK(c.cr[0] == 0);
    // store groups: note_stores before the first store and the _nb stores leave the memory and every write mark exactly as the same plain stores do
    {
        std::mt19937 rng(7);
        const uint32_t bases[] = {0x80001000u, 0x80001FF8u, 0x80001FFEu, 0x80002003u, 0xC0001FFCu, 0x90000FFFu, 0x817F0000u, 0x80002000u, 0x80003F00u, 0x93FF0000u, 0xE0001000u};
        struct Store { int op, reg; int32_t offset; };   // op: 0 stw, 1 stb, 2 sth, 3 stfs, 4 stfd
        const auto size_of = [](int op) { return op == 0 || op == 3 ? 4 : op == 1 ? 1 : op == 2 ? 2 : 8; };
        Fix g; Ctx& d = g.c;
        int tested = 0;
        for (int n = 0; n < 3000; ++n) {
            std::vector<Store> stores;
            const int count = 2 + int(rng() % 6);
            const int32_t span = (rng() % 4 == 0) ? 4000 : 200;
            int32_t lo = INT32_MAX, hi = INT32_MIN;
            for (int i = 0; i < count; ++i) {
                const Store s{int(rng() % 5), int(rng() % 8), int32_t(rng() % span) - span / 2};
                stores.push_back(s); lo = std::min(lo, s.offset); hi = std::max(hi, s.offset + size_of(s.op));
            }
            if (hi - lo > 4096) continue;   // the translator does not group stores that far apart
            ++tested;
            const uint32_t base = bases[rng() % std::size(bases)] + (rng() % 3 == 0 ? uint32_t(rng() % 64) : 0);
            const int base_reg = 3 + int(rng() % 20);   // includes r13, a small-data base that skips the MMIO test
            const uint32_t window = base + uint32_t(lo) - 16, window_size = uint32_t(hi - lo) + 32;
            std::vector<uint8_t> memory0, dirty0; std::vector<uint32_t> epochs0;
            for (int grouped = 0; grouped < 2; ++grouped) {
                for (uint32_t i = 0; i < window_size; ++i) d.mem[window + i] = uint8_t(i * 7 + n);
                for (int r = 0; r < 32; ++r) { d.r[r] = 0x01010101u * uint32_t(r + 1); d.f[r] = 1.5 * (r + 1); }
                d.r[base_reg] = base;
                gx::watch_reset(); gx::write_epoch = 17; uc::rollback_dirty.fill(0); uc::rollback_memory_base = d.mem;
                if (grouped) note_stores(d, base_reg, lo, uint32_t(hi - lo));
                for (const auto& s : stores) {
                    switch (s.op) {
                    case 0: grouped ? op_stw_nb(d, s.reg, base_reg, s.offset) : op_stw(d, s.reg, base_reg, s.offset); break;
                    case 1: grouped ? op_stb_nb(d, s.reg, base_reg, s.offset) : op_stb(d, s.reg, base_reg, s.offset); break;
                    case 2: grouped ? op_sth_nb(d, s.reg, base_reg, s.offset) : op_sth(d, s.reg, base_reg, s.offset); break;
                    case 3: grouped ? op_stfs_nb(d, s.reg, base_reg, s.offset) : op_stfs(d, s.reg, base_reg, s.offset); break;
                    default: grouped ? op_stfd_nb(d, s.reg, base_reg, s.offset) : op_stfd(d, s.reg, base_reg, s.offset); break;
                    }
                }
                uc::rollback_memory_base = nullptr;
                const std::vector<uint8_t> memory(d.mem + window, d.mem + window + window_size);
                if (!grouped) { memory0 = memory; epochs0.assign(gx::page_epoch, gx::page_epoch + gx::kWatchPages); dirty0.assign(uc::rollback_dirty.begin(), uc::rollback_dirty.end()); }
                else {
                    CHECK(memory == memory0);
                    CHECK(std::equal(epochs0.begin(), epochs0.end(), gx::page_epoch));
                    CHECK(std::equal(dirty0.begin(), dirty0.end(), uc::rollback_dirty.begin()));
                }
            }
        }
        CHECK(tested > 2000);
    }
    {   // the update forms with r1 are stack stores (the frame push of every call): the word lands at the new sp, r1 moves, the rollback mark is set; other bases keep the full barrier
        gx::watch_reset(); gx::write_epoch = 17; uc::rollback_dirty.fill(0); uc::rollback_memory_base = c.mem;
        c.r[1] = 0x80041000; c.r[5] = 0xCAFEBABE;
        op_stwu(c, 5, 1, -48);                 CHECK(c.r[1] == 0x80040FD0 && rd32(c, 0x80040FD0) == 0xCAFEBABE);
        CHECK(uc::rollback_dirty[0x40] == 1);
        c.r[1] = 0x80042000; c.r[5] = 0xAB;
        op_stbu(c, 5, 1, -3);                  CHECK(c.r[1] == 0x80041FFD && rd8(c, 0x80041FFD) == 0xAB);
        CHECK(uc::rollback_dirty[0x41] == 1);
        c.r[1] = 0x80043004; c.r[5] = 0x1234;
        op_sthu(c, 5, 1, -4);                  CHECK(c.r[1] == 0x80043000 && rd16(c, 0x80043000) == 0x1234);
        c.r[6] = 0x80050010; c.r[5] = 0x55667788;
        op_stwu(c, 5, 6, 8);                   CHECK(c.r[6] == 0x80050018 && rd32(c, 0x80050018) == 0x55667788);
        CHECK(gx::page_epoch[0x50] == 17 && uc::rollback_dirty[0x50] == 1);   // another base: the write barrier as before
        uc::rollback_memory_base = nullptr;
    }
    {   // stmw / lmw in one pass leave the memory, the registers and every write mark exactly as the word-by-word loops do (the loop is the reference, written out here)
        std::mt19937 rng(11);
        const uint32_t bases[] = {0x80001000u, 0x80001FF0u, 0x80001FFCu, 0x80002001u, 0xC0001FF8u, 0x90000FFEu, 0x817FFF00u, 0xE0001F80u, 0x93FFFF00u};
        Fix g; Ctx& d = g.c;
        for (int n = 0; n < 600; ++n) {
            const int first = 14 + int(rng() % 18), base_reg = 3 + int(rng() % 8);
            const uint32_t base = bases[rng() % std::size(bases)] + 4 * uint32_t(rng() % 4), bytes = 4u * uint32_t(32 - first);
            for (int arm = 0; arm < 2; ++arm) {
                for (uint32_t i = 0; i < 256; ++i) d.mem[base - 64 + i] = uint8_t(i * 5 + n);
                for (int r = 0; r < 32; ++r) d.r[r] = 0x01010101u * uint32_t(r + 1) + uint32_t(n);
                d.r[base_reg] = base;
                gx::watch_reset(); gx::write_epoch = 17; uc::rollback_dirty.fill(0); uc::rollback_memory_base = d.mem;
                if (arm == 0) { op_stmw(d, first, base_reg, 0); }
                else { uint32_t e = base; for (int i = first; i < 32; ++i, e += 4) wr32(d, e, d.r[i]); }
                const std::vector<uint8_t> image(d.mem + base - 64, d.mem + base - 64 + 256);
                static std::vector<uint8_t> image0, dirty0; static std::vector<uint32_t> epochs0;
                if (arm == 0) { image0 = image; dirty0.assign(uc::rollback_dirty.begin(), uc::rollback_dirty.end()); epochs0.assign(gx::page_epoch, gx::page_epoch + gx::kWatchPages); }
                else { CHECK(image == image0); CHECK(std::equal(dirty0.begin(), dirty0.end(), uc::rollback_dirty.begin())); CHECK(std::equal(epochs0.begin(), epochs0.end(), gx::page_epoch)); }
                uc::rollback_memory_base = nullptr;
            }
            // lmw: the registers
            for (int arm = 0; arm < 2; ++arm) {
                for (uint32_t i = 0; i < bytes; ++i) d.mem[base + i] = uint8_t(i * 3 + n);
                for (int r = 0; r < 32; ++r) d.r[r] = 0xA5A50000u + uint32_t(r);
                d.r[base_reg] = base;
                static uint32_t regs0[32];
                const uint32_t keep = d.r[base_reg];
                if (arm == 0) { op_lmw(d, first, base_reg, 0); for (int r = 0; r < 32; ++r) regs0[r] = d.r[r]; }
                else { uint32_t e = base; for (int i = first; i < 32; ++i, e += 4) d.r[i] = rd32(d, e); for (int r = 0; r < 32; ++r) CHECK(d.r[r] == regs0[r]); }
                (void)keep;
            }
        }
    }
    // dcbz clears the aligned 32-byte block
    for (int i = 0; i < 32; i += 4) wr32(c, 0x80002500 + i, 0xFFFFFFFF);
    c.r[3] = 0x80002500; c.r[4] = 0x14; op_dcbz(c, 3, 4);
    CHECK(rd32(c, 0x80002500) == 0 && rd32(c, 0x8000251C) == 0);
    return 0;
}
