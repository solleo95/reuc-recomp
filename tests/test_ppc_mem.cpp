#include "check.h"
#include "ppc_fixture.h"
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
    // dcbz clears the aligned 32-byte block
    for (int i = 0; i < 32; i += 4) wr32(c, 0x80002500 + i, 0xFFFFFFFF);
    c.r[3] = 0x80002500; c.r[4] = 0x14; op_dcbz(c, 3, 4);
    CHECK(rd32(c, 0x80002500) == 0 && rd32(c, 0x8000251C) == 0);
    return 0;
}
