#include "ppc/isa.h"
#include <unordered_map>
#include <vector>

namespace ppc {

#define N Sub::N
#define X5 Sub::X5
#define X6 Sub::X6
#define X9 Sub::X9
#define X10 Sub::X10
#define K(k) Kind::k

// clang-format off
#define PPC_OPS(X) \
  X(twi,3,N,0,RRS,0) X(mulli,7,N,0,RRS,0) X(subfic,8,N,0,RRS,0) X(cmpli,10,N,0,CMPU,0) X(cmpi,11,N,0,CMPS,0) \
  X(addic,12,N,0,RRS,0) X(addic_,13,N,0,RRS,0) X(addi,14,N,0,RRS,0) X(addis,15,N,0,RRS,0) \
  X(bc,16,N,0,BC,0) X(sc,17,N,0,SC,0) X(b,18,N,0,BR,0) \
  X(rlwimi,20,N,0,RRM,1) X(rlwinm,21,N,0,RRM,1) X(rlwnm,23,N,0,RRRM,1) \
  X(ori,24,N,0,RRU,0) X(oris,25,N,0,RRU,0) X(xori,26,N,0,RRU,0) X(xoris,27,N,0,RRU,0) \
  X(andi_,28,N,0,RRU,0) X(andis_,29,N,0,RRU,0) \
  X(lwz,32,N,0,RRS,0) X(lwzu,33,N,0,RRS,0) X(lbz,34,N,0,RRS,0) X(lbzu,35,N,0,RRS,0) \
  X(stw,36,N,0,RRS,0) X(stwu,37,N,0,RRS,0) X(stb,38,N,0,RRS,0) X(stbu,39,N,0,RRS,0) \
  X(lhz,40,N,0,RRS,0) X(lhzu,41,N,0,RRS,0) X(lha,42,N,0,RRS,0) X(lhau,43,N,0,RRS,0) \
  X(sth,44,N,0,RRS,0) X(sthu,45,N,0,RRS,0) X(lmw,46,N,0,RRS,0) X(stmw,47,N,0,RRS,0) \
  X(lfs,48,N,0,RRS,0) X(lfsu,49,N,0,RRS,0) X(lfd,50,N,0,RRS,0) X(lfdu,51,N,0,RRS,0) \
  X(stfs,52,N,0,RRS,0) X(stfsu,53,N,0,RRS,0) X(stfd,54,N,0,RRS,0) X(stfdu,55,N,0,RRS,0) \
  X(psq_l,56,N,0,PSQ,0) X(psq_lu,57,N,0,PSQ,0) X(psq_st,60,N,0,PSQ,0) X(psq_stu,61,N,0,PSQ,0) \
  X(mcrf,19,X10,0,MCRF,0) X(bclr,19,X10,16,BCLR,0) X(crnor,19,X10,33,CRB,0) X(rfi,19,X10,50,UNSUP,0) \
  X(crandc,19,X10,129,CRB,0) X(isync,19,X10,150,NONE,0) X(crxor,19,X10,193,CRB,0) X(crnand,19,X10,225,CRB,0) \
  X(crand,19,X10,257,CRB,0) X(creqv,19,X10,289,CRB,0) X(crorc,19,X10,417,CRB,0) X(cror,19,X10,449,CRB,0) \
  X(bcctr,19,X10,528,BCCTR,0) \
  X(cmp,31,X10,0,CMP,0) X(tw,31,X10,4,RRR,0) X(lwarx,31,X10,20,RRR,0) X(lwzx,31,X10,23,RRR,0) \
  X(slw,31,X10,24,RRR,1) X(cntlzw,31,X10,26,RR,1) X(and,31,X10,28,RRR,1) X(cmpl,31,X10,32,CMP,0) \
  X(lwzux,31,X10,55,RRR,0) X(andc,31,X10,60,RRR,1) X(mfmsr,31,X10,83,R,0) X(dcbf,31,X10,86,AB,0) X(dcbst,31,X10,54,AB,0) \
  X(lbzx,31,X10,87,RRR,0) X(lbzux,31,X10,119,RRR,0) X(nor,31,X10,124,RRR,1) X(mtcrf,31,X10,144,MTCRF,0) \
  X(mtmsr,31,X10,146,R,0) X(stwcx_,31,X10,150,RRR,0) X(stwx,31,X10,151,RRR,0) X(stwux,31,X10,183,RRR,0) \
  X(mtsr,31,X10,210,UNSUP,0) X(stbx,31,X10,215,RRR,0) X(mtsrin,31,X10,242,UNSUP,0) X(dcbtst,31,X10,246,AB,0) \
  X(stbux,31,X10,247,RRR,0) X(dcbt,31,X10,278,AB,0) X(lhzx,31,X10,279,RRR,0) X(eqv,31,X10,284,RRR,1) \
  X(tlbie,31,X10,306,UNSUP,0) X(eciwx,31,X10,310,UNSUP,0) X(lhzux,31,X10,311,RRR,0) X(xor,31,X10,316,RRR,1) \
  X(mfspr,31,X10,339,SPR,0) X(lhax,31,X10,343,RRR,0) X(mftb,31,X10,371,SPR,0) X(lhaux,31,X10,375,RRR,0) \
  X(sthx,31,X10,407,RRR,0) X(orc,31,X10,412,RRR,1) X(ecowx,31,X10,438,UNSUP,0) X(sthux,31,X10,439,RRR,0) \
  X(or,31,X10,444,RRR,1) X(mtspr,31,X10,467,SPR,0) X(dcbi,31,X10,470,AB,0) X(nand,31,X10,476,RRR,1) \
  X(mcrxr,31,X10,512,CRF,0) X(lswx,31,X10,533,UNSUP,0) X(lwbrx,31,X10,534,RRR,0) X(lfsx,31,X10,535,RRR,0) \
  X(srw,31,X10,536,RRR,1) X(tlbsync,31,X10,566,UNSUP,0) X(lfsux,31,X10,567,RRR,0) X(mfsr,31,X10,595,UNSUP,0) \
  X(lswi,31,X10,597,UNSUP,0) X(sync,31,X10,598,NONE,0) X(lfdx,31,X10,599,RRR,0) X(lfdux,31,X10,631,RRR,0) \
  X(mfsrin,31,X10,659,UNSUP,0) X(stswx,31,X10,661,UNSUP,0) X(stwbrx,31,X10,662,RRR,0) X(stfsx,31,X10,663,RRR,0) \
  X(stfsux,31,X10,695,RRR,0) X(stswi,31,X10,725,UNSUP,0) X(stfdx,31,X10,727,RRR,0) X(stfdux,31,X10,759,RRR,0) \
  X(lhbrx,31,X10,790,RRR,0) X(sraw,31,X10,792,RRR,1) X(srawi,31,X10,824,RRR,1) X(eieio,31,X10,854,NONE,0) \
  X(sthbrx,31,X10,918,RRR,0) X(extsh,31,X10,922,RR,1) X(extsb,31,X10,954,RR,1) X(icbi,31,X10,982,AB,0) \
  X(stfiwx,31,X10,983,RRR,0) X(dcbz,31,X10,1014,AB,0) X(mfcr,31,X10,19,R,0) \
  X(subfc,31,X9,8,RRR,1) X(addc,31,X9,10,RRR,1) X(mulhwu,31,X9,11,RRR,1) X(subf,31,X9,40,RRR,1) \
  X(mulhw,31,X9,75,RRR,1) X(neg,31,X9,104,RR,1) X(subfe,31,X9,136,RRR,1) X(adde,31,X9,138,RRR,1) \
  X(subfze,31,X9,200,RR,1) X(addze,31,X9,202,RR,1) X(subfme,31,X9,232,RR,1) X(addme,31,X9,234,RR,1) \
  X(mullw,31,X9,235,RRR,1) X(add,31,X9,266,RRR,1) X(divwu,31,X9,459,RRR,1) X(divw,31,X9,491,RRR,1) \
  X(fdivs,59,X5,18,RRR,1) X(fsubs,59,X5,20,RRR,1) X(fadds,59,X5,21,RRR,1) X(fsqrts,59,X5,22,RB,1) \
  X(fres,59,X5,24,RB,1) X(fmuls,59,X5,25,RAC,1) X(fmsubs,59,X5,28,RRRR,1) X(fmadds,59,X5,29,RRRR,1) \
  X(fnmsubs,59,X5,30,RRRR,1) X(fnmadds,59,X5,31,RRRR,1) \
  X(fdiv,63,X5,18,RRR,1) X(fsub,63,X5,20,RRR,1) X(fadd,63,X5,21,RRR,1) X(fsel,63,X5,23,RRRR,1) \
  X(fmul,63,X5,25,RAC,1) X(frsqrte,63,X5,26,RB,1) X(fmsub,63,X5,28,RRRR,1) X(fmadd,63,X5,29,RRRR,1) \
  X(fnmsub,63,X5,30,RRRR,1) X(fnmadd,63,X5,31,RRRR,1) \
  X(fcmpu,63,X10,0,CMP,0) X(frsp,63,X10,12,RB,1) X(fctiw,63,X10,14,RB,1) X(fctiwz,63,X10,15,RB,1) \
  X(fcmpo,63,X10,32,CMP,0) X(mtfsb1,63,X10,38,R,1) X(fneg,63,X10,40,RB,1) X(mcrfs,63,X10,64,MCRF,0) \
  X(mtfsb0,63,X10,70,R,1) X(fmr,63,X10,72,RB,1) X(mtfsfi,63,X10,134,MCRF,1) X(fnabs,63,X10,136,RB,1) \
  X(fabs,63,X10,264,RB,1) X(mffs,63,X10,583,R,1) X(mtfsf,63,X10,711,MTCRF,1) \
  X(ps_sum0,4,X5,10,RRRR,1) X(ps_sum1,4,X5,11,RRRR,1) X(ps_muls0,4,X5,12,RAC,1) X(ps_muls1,4,X5,13,RAC,1) \
  X(ps_madds0,4,X5,14,RRRR,1) X(ps_madds1,4,X5,15,RRRR,1) X(ps_div,4,X5,18,RRR,1) X(ps_sub,4,X5,20,RRR,1) \
  X(ps_add,4,X5,21,RRR,1) X(ps_sel,4,X5,23,RRRR,1) X(ps_res,4,X5,24,RB,1) X(ps_mul,4,X5,25,RAC,1) \
  X(ps_rsqrte,4,X5,26,RB,1) X(ps_msub,4,X5,28,RRRR,1) X(ps_madd,4,X5,29,RRRR,1) X(ps_nmsub,4,X5,30,RRRR,1) \
  X(ps_nmadd,4,X5,31,RRRR,1) \
  X(psq_lx,4,X6,6,PSQX,0) X(psq_stx,4,X6,7,PSQX,0) X(psq_lux,4,X6,38,PSQX,0) X(psq_stux,4,X6,39,PSQX,0) \
  X(ps_cmpu0,4,X10,0,CMP,0) X(ps_cmpo0,4,X10,32,CMP,0) X(ps_cmpu1,4,X10,64,CMP,0) X(ps_cmpo1,4,X10,96,CMP,0) \
  X(ps_neg,4,X10,40,RB,1) X(ps_mr,4,X10,72,RB,1) X(ps_nabs,4,X10,136,RB,1) X(ps_abs,4,X10,264,RB,1) \
  X(ps_merge00,4,X10,528,RRR,1) X(ps_merge01,4,X10,560,RRR,1) X(ps_merge10,4,X10,592,RRR,1) \
  X(ps_merge11,4,X10,624,RRR,1) X(dcbz_l,4,X10,1014,AB,0)
// clang-format on

static const OpInfo kOps[] = {
#define X(nm, p, s, xo, k, r) {#nm, p, s, xo, K(k), r != 0},
    PPC_OPS(X)
#undef X
};
// The table macros must not leak into the code below (`Sub::X5` would otherwise expand twice).
#undef N
#undef X5
#undef X6
#undef X9
#undef X10
#undef K

static uint32_t key(uint8_t p, Sub s, uint32_t xo) { return (uint32_t(p) << 16) | (uint32_t(s) << 12) | xo; }

static const std::unordered_map<uint32_t, const OpInfo*>& table() {
    static const auto m = [] {
        std::unordered_map<uint32_t, const OpInfo*> t;
        for (const auto& o : kOps) t[key(o.primary, o.sub, o.xo)] = &o;
        return t;
    }();
    return m;
}

static const OpInfo* find(uint8_t p, Sub s, uint32_t xo) {
    auto& t = table();
    auto it = t.find(key(p, s, xo));
    return it == t.end() ? nullptr : it->second;
}

const OpInfo* decode(uint32_t w) {
    uint8_t p = uint8_t(w >> 26);
    uint32_t x5 = (w >> 1) & 0x1F, x6 = (w >> 1) & 0x3F, x10 = (w >> 1) & 0x3FF;
    switch (p) {
        case 4:
            if (auto* o = find(4, Sub::X5, x5)) return o;
            if (auto* o = find(4, Sub::X6, x6)) return o;
            return find(4, Sub::X10, x10);
        case 59: return find(59, Sub::X5, x5);
        case 63:
            if (auto* o = find(63, Sub::X5, x5)) return o;
            return find(63, Sub::X10, x10);
        case 19: return find(19, Sub::X10, x10);
        case 31:
            if (auto* o = find(31, Sub::X10, x10)) return o;
            return find(31, Sub::X9, x10 & 0x1FF);
        default: return find(p, Sub::N, 0);
    }
}

const OpInfo* const* all_ops(size_t& count) {
    static const std::vector<const OpInfo*> v = [] {
        std::vector<const OpInfo*> r;
        for (const auto& o : kOps) r.push_back(&o);
        return r;
    }();
    count = v.size();
    return v.data();
}

}  // namespace ppc
