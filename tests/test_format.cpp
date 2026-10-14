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
    CHECK(ppc::format_insn(0, 0x7C7E00D0) == "neg r3,r30");
    for (auto [xo, name] : {std::pair{200u, "subfze"}, {202u, "addze"},
                           {232u, "subfme"}, {234u, "addme"}})
        CHECK(ppc::format_insn(0, (31u<<26)|(3u<<21)|(30u<<16)|(xo<<1)) ==
              std::string(name) + " r3,r30");
    CHECK(ppc::format_insn(0, (31u<<26)|(4u<<21)|(3u<<16)|(26u<<1)) == "cntlzw r3,r4");
    CHECK(ppc::format_insn(0, (59u<<26)|(1u<<21)|(2u<<16)|(3u<<11)|(20u<<1)) == "fsubs f1,f2,f3");
    CHECK(ppc::format_insn(0, (63u<<26)|(1u<<21)|(2u<<16)|(3u<<11)|(21u<<1)) == "fadd f1,f2,f3");
    CHECK(ppc::format_insn(0, (4u<<26)|(1u<<21)|(2u<<16)|(3u<<11)|(21u<<1)) == "ps_add f1,f2,f3");
    CHECK(ppc::format_insn(0, (63u<<26)|(2u<<16)|(3u<<11)|(32u<<1)) == "fcmpo cr0,f2,f3");
    CHECK(ppc::format_insn(0, (4u<<26)|(2u<<16)|(3u<<11)) == "ps_cmpu0 cr0,f2,f3");
    CHECK(ppc::format_insn(0, (31u<<26)|(1u<<21)|(2u<<16)|(3u<<11)|(535u<<1)) == "lfsx f1,r2,r3");
    CHECK(ppc::format_insn(0x80003100, 0x14000000).rfind(".long 0x14000000", 0) == 0);
    return 0;
}
