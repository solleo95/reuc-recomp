#pragma once
#include "dol_builder.h"
#include "ppc_enc.h"
#include "ppc_rt.h"
#include <cstring>
#include <vector>

namespace e2e {
using namespace enc;

struct Program {
    const char* name;
    std::vector<uint8_t> (*build)();
    void (*init)(ppc::Ctx&);
    bool (*check)(ppc::Ctx&);
};

inline std::vector<uint8_t> loop_dol() {
    Asm a;
    a.emit(li(3, 0)); a.emit(li(5, 0)); a.emit(li(4, 10)); a.emit(mtctr(4));
    uint32_t top = a.here();
    a.emit(addi(5, 5, 1)); a.emit(add(3, 3, 5)); a.bc_to(16, 0, top); a.emit(blr);
    return make_dol(a.w);
}

inline std::vector<uint8_t> calls_dol() {
    Asm a;
    a.emit(stwu(1, -16, 1)); a.emit(mflr(0)); a.emit(stw(0, 20, 1)); a.emit(li(3, 1));
    size_t c1 = a.emit(0);
    a.emit(lwz(0, 20, 1)); a.emit(addi(1, 1, 16)); a.emit(mtlr(0)); a.emit(blr);
    uint32_t f = a.here();
    a.emit(stwu(1, -16, 1)); a.emit(mflr(0)); a.emit(stw(0, 20, 1));
    size_t c2 = a.emit(0), c3 = a.emit(0);
    a.emit(lwz(0, 20, 1)); a.emit(addi(1, 1, 16)); a.emit(mtlr(0)); a.emit(blr);
    uint32_t g = a.here();
    a.emit(addi(3, 3, 10)); a.emit(blr);
    a.patch_bl(c1, f); a.patch_bl(c2, g); a.patch_bl(c3, g);
    return make_dol(a.w);
}

template <int Sel> std::vector<uint8_t> jt_dol() {
    Asm a;
    a.emit(li(3, Sel)); a.emit(cmplwi(3, 2));
    size_t bg = a.emit(0);
    a.emit(lis(4, 0x8040)); a.emit(addi(4, 4, 0)); a.emit(slwi(0, 3, 2)); a.emit(lwzx(0, 4, 0)); a.emit(mtctr(0)); a.emit(bctr);
    uint32_t c0 = a.here(); a.emit(li(3, 100)); a.emit(blr);
    uint32_t c1 = a.here(); a.emit(li(3, 200)); a.emit(blr);
    uint32_t c2 = a.here(); a.emit(li(3, 300)); a.emit(blr);
    uint32_t d = a.here();  a.emit(li(3, -1)); a.emit(blr);
    a.patch_bc(bg, 12, 1, d);
    std::vector<uint8_t> data; be32(data, c0); be32(data, c1); be32(data, c2);
    return make_dol(a.w, data);
}

inline std::vector<uint8_t> indirect_dol() {
    Asm a;
    a.emit(lis(4, 0x8040)); a.emit(addi(4, 4, 0)); a.emit(lwz(12, 0, 4)); a.emit(mtctr(12)); a.emit(0x4E800421u);  // bctrl
    a.emit(addi(3, 3, 1)); a.emit(blr);
    uint32_t callee = a.here(); a.emit(li(3, 77)); a.emit(blr);
    std::vector<uint8_t> data; be32(data, callee);
    return make_dol(a.w, data);
}

inline std::vector<uint8_t> float_dol() {
    Asm a;
    a.emit(lis(4, 0x8040)); a.emit(lfs(1, 0, 4)); a.emit(fadds(2, 1, 1)); a.emit(fctiwz(3, 2));
    a.emit(lis(5, 0x8050)); a.emit(li(6, 0)); a.emit(stfiwx(3, 5, 6)); a.emit(lwz(3, 0, 5)); a.emit(blr);
    std::vector<uint8_t> data; be32(data, 0x3FC00000);   // 1.5f
    return make_dol(a.w, data);
}

inline std::vector<uint8_t> ps_dol() {
    Asm a;
    a.emit(lis(4, 0x8040)); a.emit(psq_l(1, 0, 4, 0, 0)); a.emit(ps_add(2, 1, 1)); a.emit(lis(5, 0x8050));
    a.emit(psq_st(2, 0, 5, 0, 0)); a.emit(lwz(3, 0, 5)); a.emit(lwz(6, 4, 5)); a.emit(blr);
    std::vector<uint8_t> data; be32(data, 0x3F800000); be32(data, 0x40000000);   // 1.0f, 2.0f
    return make_dol(a.w, data);
}

inline std::vector<uint8_t> datawords_dol() {
    Asm a;
    a.emit(li(3, 5));
    size_t br = a.emit(0);
    a.emit(0x14000000u); a.emit(0x00000000u);            // illegal (primary 5) and zero words, never executed
    uint32_t skip = a.here();
    a.emit(addi(3, 3, 1)); a.emit(blr);
    a.patch_b(br, skip);
    return make_dol(a.w);
}

inline std::vector<uint8_t> badbctr_dol() {
    Asm a;
    a.emit(lis(12, 0x8000)); a.emit(ori(12, 12, 0x3FFC)); a.emit(mtctr(12)); a.emit(0x4E800421u); a.emit(blr);
    return make_dol(a.w);
}

inline std::vector<uint8_t> illegal_exec_dol() {
    Asm a;
    a.emit(li(3, 5)); a.emit(0x14000000u); a.emit(blr);
    return make_dol(a.w);
}

// A branch into the middle of another function runs the code from there (secondary entry), not from the function start.
inline std::vector<uint8_t> secondary_dol() {
    Asm a;
    size_t c1 = a.emit(0);                 // bl G   (r3 = 99 + 1)
    a.emit(li(3, 5));
    size_t br = a.emit(0);                 // b Gmid (skips "li r3,99")
    a.emit(blr);
    uint32_t g = a.here();
    a.emit(li(3, 99));
    uint32_t gmid = a.here();
    a.emit(addi(3, 3, 1)); a.emit(blr);
    a.patch_bl(c1, g); a.patch_b(br, gmid);
    return make_dol(a.w);
}

// A function whose end falls into the next function (an entry because it is called) continues there.
inline std::vector<uint8_t> fallthrough_dol() {
    Asm a;
    size_t c1 = a.emit(0);                 // bl F
    a.emit(mr(4, 3)); a.emit(li(3, 0));
    size_t c2 = a.emit(0);                 // bl G
    a.emit(add(3, 3, 4)); a.emit(blr);
    uint32_t f = a.here(); a.emit(li(3, 1));
    uint32_t g = a.here(); a.emit(addi(3, 3, 41)); a.emit(blr);
    a.patch_bl(c1, f); a.patch_bl(c2, g);
    return make_dol(a.w);
}

inline std::vector<uint8_t> outside_dol() {
    Asm a;
    a.emit(b(0x100000)); a.emit(blr);
    return make_dol(a.w);
}

template <uint32_t Expect> bool r3_is(ppc::Ctx& c) { return c.r[3] == Expect; }

inline const std::vector<Program>& all() {
    static const std::vector<Program> v = {
        {"loop", loop_dol, nullptr, r3_is<55>},
        {"calls", calls_dol, nullptr, [](ppc::Ctx& c) { return c.r[3] == 21 && c.r[1] == 0x817FFF00; }},
        {"jt0", jt_dol<0>, nullptr, r3_is<100>},
        {"jt1", jt_dol<1>, nullptr, r3_is<200>},
        {"jt2", jt_dol<2>, nullptr, r3_is<300>},
        {"jt9", jt_dol<9>, nullptr, r3_is<0xFFFFFFFFu>},
        {"indirect", indirect_dol, nullptr, r3_is<78>},
        {"float", float_dol, nullptr, r3_is<3>},
        {"ps", ps_dol, nullptr, [](ppc::Ctx& c) { return c.r[3] == 0x40000000 && c.r[6] == 0x40800000; }},
        {"datawords", datawords_dol, nullptr, r3_is<6>},
        {"badbctr", badbctr_dol, nullptr, [](ppc::Ctx&) { return false; }},
        {"illegal_exec", illegal_exec_dol, nullptr, [](ppc::Ctx&) { return false; }},
        {"secondary", secondary_dol, nullptr, r3_is<6>},
        {"fallthrough", fallthrough_dol, nullptr, r3_is<83>},
        {"outside", outside_dol, nullptr, [](ppc::Ctx&) { return false; }},
    };
    return v;
}
inline const Program* find(const char* name) {
    for (auto& p : all()) if (!std::strcmp(p.name, name)) return &p;
    return nullptr;
}
}  // namespace e2e
