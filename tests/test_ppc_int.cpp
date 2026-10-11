#include "check.h"
#include "ppc_fixture.h"
using namespace ppc;

int main() {
    Fix f; Ctx& c = f.c;

    // add / carry chain: 0x1_FFFFFFFF + 1 = 0x2_00000000 via addc/adde
    c.r[3] = 0xFFFFFFFF; c.r[4] = 1; c.r[5] = 1; c.r[6] = 0;
    op_addc(c, 7, 3, 4, false);
    CHECK(c.r[7] == 0 && c.xer_ca == 1);        // low word 0, carry out
    op_adde(c, 8, 5, 6, false);
    CHECK(c.r[8] == 2 && c.xer_ca == 0);        // 1 + 0 + carry
    // add with Rc: zero result -> CR0 = EQ
    c.r[3] = 0xFFFFFFFF; c.r[4] = 1;
    op_add(c, 5, 3, 4, true);
    CHECK(c.r[5] == 0 && c.cr[0] == 2);
    // addi with rA=0 uses literal 0; addis shifts
    c.r[0] = 1234;
    op_addi(c, 3, 0, 16);       CHECK(c.r[3] == 16);
    op_addis(c, 3, 0, -1);      CHECK(c.r[3] == 0xFFFF0000);
    // subf: rD = rB - rA ; subfic carry
    c.r[4] = 3; c.r[5] = 10;
    op_subf(c, 3, 4, 5, false); CHECK(c.r[3] == 7);
    c.r[4] = 3;  op_subfic(c, 3, 4, 5); CHECK(c.r[3] == 2 && c.xer_ca == 1);
    c.r[4] = 7;  op_subfic(c, 3, 4, 5); CHECK(c.r[3] == 0xFFFFFFFE && c.xer_ca == 0);
    // neg
    c.r[4] = 1; op_neg(c, 3, 4, false); CHECK(c.r[3] == 0xFFFFFFFF);
    // multiply
    c.r[4] = uint32_t(-2); c.r[5] = 3;
    op_mullw(c, 3, 4, 5, false);  CHECK(c.r[3] == 0xFFFFFFFA);
    op_mulhw(c, 3, 4, 5, false);  CHECK(c.r[3] == 0xFFFFFFFF);
    c.r[4] = 0xFFFFFFFF; c.r[5] = 2;
    op_mulhwu(c, 3, 4, 5, false); CHECK(c.r[3] == 1);
    op_mulli(c, 3, 5, -3);        CHECK(c.r[3] == uint32_t(-6));
    // divide: truncating; divide by zero / overflow follow the hardware-observed convention
    c.r[4] = 7; c.r[5] = 2;           op_divw(c, 3, 4, 5, false);  CHECK(c.r[3] == 3);
    c.r[4] = uint32_t(-7);            op_divw(c, 3, 4, 5, false);  CHECK(c.r[3] == uint32_t(-3));
    c.r[4] = 7; c.r[5] = 0;           op_divw(c, 3, 4, 5, false);  CHECK(c.r[3] == 0);
    c.r[4] = uint32_t(-7);            op_divw(c, 3, 4, 5, false);  CHECK(c.r[3] == 0xFFFFFFFF);
    c.r[4] = 0x80000000; c.r[5] = uint32_t(-1); op_divw(c, 3, 4, 5, false); CHECK(c.r[3] == 0xFFFFFFFF);
    c.r[4] = 7; c.r[5] = 0;           op_divwu(c, 3, 4, 5, false); CHECK(c.r[3] == 0);
    c.r[4] = 0xFFFFFFFE; c.r[5] = 2;  op_divwu(c, 3, 4, 5, false); CHECK(c.r[3] == 0x7FFFFFFF);
    // logical (rs=3, rb=4, ra=5)
    c.r[3] = 0xF0F0F0F0; c.r[4] = 0xFF00FF00;
    op_and(c, 3, 5, 4, false);  CHECK(c.r[5] == 0xF000F000);
    op_or(c, 3, 5, 4, false);   CHECK(c.r[5] == 0xFFF0FFF0);
    op_xor(c, 3, 5, 4, false);  CHECK(c.r[5] == 0x0FF00FF0);
    op_nor(c, 3, 5, 4, false);  CHECK(c.r[5] == 0x000F000F);
    op_nand(c, 3, 5, 4, false); CHECK(c.r[5] == 0x0FFF0FFF);
    op_eqv(c, 3, 5, 4, false);  CHECK(c.r[5] == 0xF00FF00F);
    op_andc(c, 3, 5, 4, false); CHECK(c.r[5] == 0x00F000F0);
    op_orc(c, 3, 5, 4, false);  CHECK(c.r[5] == 0xF0FFF0FF);
    op_andi_(c, 3, 5, 0x00FF);  CHECK(c.r[5] == 0xF0 && c.cr[0] == 4);
    op_oris(c, 3, 5, 0x1234);   CHECK(c.r[5] == (0xF0F0F0F0u | 0x12340000u));
    op_xori(c, 3, 5, 0xFFFF);   CHECK(c.r[5] == (0xF0F0F0F0u ^ 0xFFFFu));
    // extend / count leading zeros
    c.r[3] = 0x80; op_extsb(c, 3, 5, false); CHECK(c.r[5] == 0xFFFFFF80);
    c.r[3] = 0x8000; op_extsh(c, 3, 5, false); CHECK(c.r[5] == 0xFFFF8000);
    c.r[3] = 0; op_cntlzw(c, 3, 5, false); CHECK(c.r[5] == 32);
    c.r[3] = 1; op_cntlzw(c, 3, 5, false); CHECK(c.r[5] == 31);
    c.r[3] = 0x80000000; op_cntlzw(c, 3, 5, false); CHECK(c.r[5] == 0);
    // shifts (rs=3, rb=4, ra=5); shift counts use 6 bits, >= 32 gives 0
    c.r[3] = 1; c.r[4] = 4;  op_slw(c, 3, 5, 4, false); CHECK(c.r[5] == 16);
    c.r[4] = 32;             op_slw(c, 3, 5, 4, false); CHECK(c.r[5] == 0);
    c.r[3] = 0x80000000; c.r[4] = 31; op_srw(c, 3, 5, 4, false); CHECK(c.r[5] == 1);
    c.r[4] = 32;             op_srw(c, 3, 5, 4, false); CHECK(c.r[5] == 0);
    c.r[3] = uint32_t(-5); c.r[4] = 1; op_sraw(c, 3, 5, 4, false); CHECK(c.r[5] == uint32_t(-3) && c.xer_ca == 1);
    c.r[3] = uint32_t(-4);             op_sraw(c, 3, 5, 4, false); CHECK(c.r[5] == uint32_t(-2) && c.xer_ca == 0);
    c.r[3] = uint32_t(-4); c.r[4] = 40; op_sraw(c, 3, 5, 4, false); CHECK(c.r[5] == 0xFFFFFFFF && c.xer_ca == 1);
    c.r[3] = 0xFFFFFFF1;  op_srawi(c, 3, 5, 2, false); CHECK(c.r[5] == 0xFFFFFFFC && c.xer_ca == 1);
    // rotates: slwi, srwi, field extract, wrap-around mask, insert
    c.r[4] = 1;           op_rlwinm(c, 4, 3, 2, 0, 29, false);  CHECK(c.r[3] == 4);
    c.r[4] = 0x12345678;  op_rlwinm(c, 4, 3, 24, 8, 31, false); CHECK(c.r[3] == 0x00123456);
    c.r[4] = 0xFFFFFFFF;  op_rlwinm(c, 4, 3, 0, 16, 23, false); CHECK(c.r[3] == 0x0000FF00);
    c.r[4] = 0xFFFFFFFF;  op_rlwinm(c, 4, 3, 0, 28, 3, false);  CHECK(c.r[3] == 0xF000000F);
    c.r[5] = 0xAAAAAAAA; c.r[4] = 0x0000000F; op_rlwimi(c, 4, 5, 4, 24, 27, false); CHECK(c.r[5] == 0xAAAAAAFA);
    c.r[4] = 0x80000001; c.r[6] = 36; op_rlwnm(c, 4, 3, 6, 0, 31, false); CHECK(c.r[3] == 0x00000018);  // rotl by 36&31=4
    // compares set the chosen CR field and copy XER[SO]
    c.xer_so = 0;
    c.r[3] = 0xFFFFFFFF;
    op_cmpi(c, 1, 3, 0);  CHECK(c.cr[1] == 8);   // -1 < 0
    op_cmpli(c, 2, 3, 0); CHECK(c.cr[2] == 4);   // 0xFFFFFFFF > 0 unsigned
    c.r[4] = 0xFFFFFFFF;
    op_cmp(c, 3, 3, 4);   CHECK(c.cr[3] == 2);
    op_cmpl(c, 3, 3, 4);  CHECK(c.cr[3] == 2);
    c.xer_so = 1; op_cmpi(c, 4, 3, 0); CHECK(c.cr[4] == (8 | 1)); c.xer_so = 0;
    // CR moves and logic
    c.r[3] = 0x12345678; op_mtcrf(c, 0xFF, 3);
    CHECK(c.cr[0] == 1 && c.cr[1] == 2 && c.cr[2] == 3 && c.cr[3] == 4 && c.cr[4] == 5 && c.cr[7] == 8);
    op_mfcr(c, 5); CHECK(c.r[5] == 0x12345678);
    c.cr[1] = 0; op_crxor(c, 6, 6, 6); CHECK(c.cr[1] == 0);   // bit 6 = CR1.EQ
    op_creqv(c, 6, 6, 6);               CHECK(c.cr[1] == 2);
    c.cr[0] = 8; op_mcrf(c, 5, 0);      CHECK(c.cr[5] == 8);
    // SPR access: LR, CTR, XER, GQR, generic
    c.r[3] = 0xDEADBEE0; op_mtspr(c, 3, 8);  CHECK(c.lr == 0xDEADBEE0);
    op_mfspr(c, 4, 8);                       CHECK(c.r[4] == 0xDEADBEE0);
    c.r[3] = 77; op_mtspr(c, 3, 9);          CHECK(c.ctr == 77);
    c.r[3] = 0xA0000000; op_mtspr(c, 3, 1);  CHECK(c.xer_so == 1 && c.xer_ca == 1 && c.xer_ov == 0);
    op_mfspr(c, 4, 1);                       CHECK(c.r[4] == 0xA0000000);
    c.r[3] = 0x00070000; op_mtspr(c, 3, 912); CHECK(c.gqr[0] == 0x00070000);
    c.r[3] = 5; op_mtspr(c, 3, 1008);         op_mfspr(c, 4, 1008); CHECK(c.r[4] == 5);  // HID0 via generic array
    op_mftb(c, 4, 268);  CHECK(c.r[4] == 0x55667788);   // TBL
    op_mftb(c, 4, 269);  CHECK(c.r[4] == 0x11223344);   // TBU
    return 0;
}
