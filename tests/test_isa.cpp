#include "check.h"
#include "ppc/insn.h"
#include "ppc/isa.h"
#include <cstring>

static const char* name(uint32_t w) { auto* o = ppc::decode(w); return o ? o->name : "?"; }

int main() {
    CHECK(!std::strcmp(name(0x38610010), "addi"));    // addi r3,r1,16
    CHECK(!std::strcmp(name(0x80010014), "lwz"));     // lwz r0,20(r1)
    CHECK(!std::strcmp(name(0x9421FFF0), "stwu"));    // stwu r1,-16(r1)
    CHECK(!std::strcmp(name(0x7C0802A6), "mfspr"));   // mflr r0
    CHECK(!std::strcmp(name(0x7C0803A6), "mtspr"));   // mtlr r0
    CHECK(!std::strcmp(name(0x4E800020), "bclr"));    // blr
    CHECK(!std::strcmp(name(0x4E800420), "bcctr"));   // bctr
    CHECK(!std::strcmp(name(0x48000009), "b"));       // bl +8
    CHECK(!std::strcmp(name(0x7C642A14), "add"));     // add r3,r4,r5
    CHECK(!std::strcmp(name(0x7C643A15 | 0x400), "add"));  // addo. : OE variant resolves via the 9-bit xo
    CHECK(!std::strcmp(name(0x2C030000), "cmpi"));    // cmpwi r3,0
    CHECK(!std::strcmp(name(0x5483103A), "rlwinm"));  // slwi r3,r4,2
    CHECK(!std::strcmp(name(0x1022182A), "ps_add"));  // ps_add f1,f2,f3
    CHECK(!std::strcmp(name(0xE0230008), "psq_l"));   // psq_l f1,8(r3),0,0
    CHECK(!std::strcmp(name(0xFC22182A), "fadd"));    // fadd f1,f2,f3
    CHECK(!std::strcmp(name(0xFC201090), "fmr"));     // fmr f1,f2
    CHECK(!std::strcmp(name(0xC0230000), "lfs"));     // lfs f1,0(r3)
    CHECK(!std::strcmp(name(0xEC22182A), "fadds"));   // fadds f1,f2,f3
    CHECK(ppc::decode(0x00000000) == nullptr);        // padding
    CHECK(ppc::decode(0x0FE00000) != nullptr);        // twi 31,0,0 (trap)

    // field extractors
    uint32_t w = 0x9421FFF0;
    CHECK(ppc::t(w) == 1 && ppc::a(w) == 1 && ppc::simm(w) == -16);
    CHECK(ppc::li(0x4BFFFFFC) == -4);                 // b -4
    CHECK(ppc::bd(0x4182FFF8) == -8);                 // beq -8
    CHECK(ppc::spr(0x7C0802A6) == 8);                 // LR
    CHECK(ppc::spr(0x7C0903A6) == 9);                 // CTR
    CHECK(ppc::mb(0x5483103A) == 0 && ppc::me(0x5483103A) == 29 && ppc::sh(0x5483103A) == 2);

    size_t n = 0;
    auto* const* ops = ppc::all_ops(n);
    CHECK(n > 200);
    for (size_t i = 0; i < n; ++i) CHECK(ops[i]->name != nullptr);
    return 0;
}
