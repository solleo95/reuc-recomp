#include "check.h"
#include "ppc/format.h"

int main() {
    CHECK(ppc::format_insn(0x80003100, 0x38610010) == "addi r3,r1,16");
    CHECK(ppc::format_insn(0x80003100, 0x9421FFF0) == "stwu r1,-16(r1)");
    CHECK(ppc::format_insn(0x80003100, 0x80010014) == "lwz r0,20(r1)");
    CHECK(ppc::format_insn(0x80003100, 0x7C0802A6) == "mflr r0");
    CHECK(ppc::format_insn(0x80003100, 0x7C0803A6) == "mtlr r0");
    CHECK(ppc::format_insn(0x80003100, 0x4E800020) == "blr");
    CHECK(ppc::format_insn(0x80003100, 0x4E800420) == "bctr");
    CHECK(ppc::format_insn(0x80003100, 0x48000009) == "bl 0x80003108");
    CHECK(ppc::format_insn(0x80003100, 0x4BFFFFFC) == "b 0x800030FC");
    CHECK(ppc::format_insn(0x80003100, 0x41820010) == "bc 12,2,0x80003110");
    CHECK(ppc::format_insn(0x80003100, 0x7C642A14) == "add r3,r4,r5");
    CHECK(ppc::format_insn(0x80003100, 0x7C642A15) == "add. r3,r4,r5");
    CHECK(ppc::format_insn(0x80003100, 0x2C030000) == "cmpi cr0,r3,0");
    CHECK(ppc::format_insn(0x80003100, 0x5483103A) == "rlwinm r3,r4,2,0,29");
    CHECK(ppc::format_insn(0x80003100, 0xC0230008) == "lfs f1,8(r3)");
    // logical and shift ops: destination is rA, sources are rS and rB
    CHECK(ppc::format_insn(0x80003100, 0x7C7F1B78) == "or r31,r3,r3");
    CHECK(ppc::format_insn(0x80003100, 0x7C832830) == "slw r3,r4,r5");
    CHECK(ppc::format_insn(0x80003100, 0x7C832E70) == "srawi r3,r4,5");
    CHECK(ppc::format_insn(0x80003100, 0x7C832838) == "and r3,r4,r5");
    CHECK(ppc::format_insn(0x80003100, 0x14000000).rfind(".long 0x14000000", 0) == 0);
    return 0;
}
