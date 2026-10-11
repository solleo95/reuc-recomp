#pragma once
#include <cstddef>
#include <cstdint>

namespace ppc {

// Which instruction fields are passed to op_<name>(Ctx&, ...) in this order; `rc` appends a trailing bool.
enum class Kind : uint8_t {
    NONE,   // ()
    RRR,    // (t, a, b)
    RRS,    // (t, a, simm)
    RRU,    // (t, a, uimm)
    RR,     // (t, a)
    RB,     // (t, b)
    R,      // (t)
    AB,     // (a, b)   cache-control forms: dcbf dcbt dcbtst dcbi icbi dcbz dcbz_l
    CRF,    // (crfd)   mcrxr
    RRM,    // (t, a, sh, mb, me)
    RRRM,   // (t, a, b, mb, me)
    CMP,    // (crfd, a, b)
    CMPS,   // (crfd, a, simm)
    CMPU,   // (crfd, a, uimm)
    CRB,    // (t, a, b) used as CR bit indices
    MCRF,   // (crfd, crfs)
    SPR,    // (t, spr)
    MTCRF,  // (crm, t)
    RRRR,   // (t, a, b, c)
    RAC,    // (t, a, c)
    PSQ,    // (t, a, d12, w, i)
    PSQX,   // (t, a, b, w, i)
    BR, BC, BCLR, BCCTR, SC,  // control flow, handled by the translator
    UNSUP,  // privileged/rare: translator emits a run-time trap
};

enum class Sub : uint8_t { N, X5, X6, X9, X10 };

struct OpInfo {
    const char* name;  // '.' replaced by '_' (andi. -> andi_)
    uint8_t primary;
    Sub sub;
    uint16_t xo;
    Kind kind;
    bool rc;           // the Rc bit is meaningful and passed as the last argument
};

const OpInfo* decode(uint32_t word);
const OpInfo* const* all_ops(size_t& count);

}  // namespace ppc
