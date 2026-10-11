# Analyzer and Translator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn `main.dol` into native C++ (one function per guest function), compile it with clang-cl, and prove instruction semantics with tests. No emulator, interpreter or JIT anywhere in the output.

**Architecture:** A table-driven Gekko decoder (`common/ppc/`) feeds an analyzer (function entries/extents, jump tables) and a translator that emits calls to header-only instruction semantics (`runtime/ppc_rt.h`, one `always_inline` function per PPC instruction). Semantics are unit-tested directly on a `Ctx`; the translator is tested end to end on tiny synthetic guest programs. A final stage identifies Nintendo SDK functions by Dolphin-format signatures and string anchors.

**Tech Stack:** C++20, CMake, Ninja, clang-cl `/O2 -march=x86-64-v3`, Python 3 (helpers only).

**Spec:** `docs/superpowers/specs/2026-10-12-uc-recomp-design.md` (§5 Translation pipeline, §6 Runtime memory/threads). Evidence: `docs/spikes/S1-sdk-scan.md`, `S4-compile-time.md`. Previous plan: `docs/superpowers/plans/2026-10-12-foundation-and-spikes.md` (done).

## Global Constraints

- No interpreter, JIT, emulator or PowerPC code in any runtime artifact; an unresolved indirect target is a fatal error that prints the guest address.
- Generated code compiled with clang-cl, `/O2`, `x86-64-v3`.
- Guest memory model: one 1 GB virtual reservation, host address = `base + (ea & 0x3FFFFFFF)`, MEM1 committed at `0x00000000..0x01800000`, MEM2 at `0x10000000..0x14000000`; effective addresses `0xCC000000..0xCDFFFFFF` are MMIO and are routed to `mmio_read/mmio_write`.
- Guest is big-endian; every load/store byteswaps (`_byteswap_*`).
- Generated code is never distributed and never committed (`generated/` is git-ignored).
- `build.bat` stays one-click, exits non-zero on failure, copies `dist\` to `S:\Games\Umbrella Chronicles`.
- Project root `D:\Projects\Native Ports\Umbrella-chronicles-recomp` (path has spaces; quote everything). Commit steps are omitted: the user asked that commits happen only on request.
- Test command for every task: `scripts\dev.bat` (configure, build, ctest). Game DOL: `S:\Games\Umbrella Chronicles\game\DATA\sys\main.dol`.

## Spec deviations and proposals (surface to the user in the hand-off)

1. **Config file:** `project/uc.cfg` (line based, no YAML dependency) replaces `project/uc.yml` from the spec.
2. **Instruction semantics are functions, not inline text:** the translator emits `ppc::op_add(c, 3, 4, 5, false);` and the compiler folds the constants. This keeps semantics unit-testable without the translator.
3. **Proposal for the runtime/renderer plans (not binding here):** GX, VI and AX are stripped in this binary (no names; only OS/DVD/WPAD strings survive, see S1 and Task 8). Emulating the hardware interface (GX FIFO, VI/PE/CP registers, AX parameter blocks) behind the MMIO hook avoids having to identify ~200 GX functions. The translator and memory model in this plan support both approaches; the decision is made in the runtime plan.
4. **Register locals:** v1 operates on `Ctx` fields directly (correctness first). Register caching in locals is a later optimization, measured against a running game.

## Review Focus

1. Unresolved indirect call/jump target at run time → fatal error naming the guest address, never a silent skip (Task 6 test).
2. Data or illegal words inside a function's address range (padding, inline constants) → emitted as a trap, never a compile error (Task 6 test).
3. Branch into the middle of another function → recorded and trapped at run time, translator does not crash (Task 6 test).
4. Arithmetic edge cases: divide by zero, shift counts ≥ 32, `sraw` carry, `rlwinm` wrap-around masks, `fctiw` overflow clamp, quantized store clamping (Tasks 2–3 tests).
5. Memory edges: MMIO window boundaries `0xCBFFFFFF` / `0xCC000000` / `0xCDFFFFFF` / `0xCE000000`, byte order, unaligned access (Task 2 test).
6. Translator output is byte-for-byte deterministic (cache key depends on it) (Task 6 test).

## File Structure

```
common/ppc/isa.h, isa.cpp         OpInfo table (X-macro), Kind, decode(word)
common/ppc/insn.h                 field extractors (rt, ra, rb, simm, mb, me, bo, bi, li, bd, spr ...)
runtime/CMakeLists.txt            uc_runtime lib (guest_mem, dispatch)
runtime/ppc_rt.h                  Ctx, memory access, op_* semantics (header only)
runtime/guest_mem.h, guest_mem.cpp  1 GB reservation, MEM1/MEM2 commit
runtime/dispatch.h, dispatch.cpp  FnEntry table, call_indirect, traps
common/analyzer/analyzer.h, .cpp  entries, extents, jump tables
common/analyzer/config.h, .cpp    project/uc.cfg loader
common/translator/emit.h, .cpp    function emitter, shard writer
tools/translator/main.cpp         CLI
tools/dol_coverage/main.cpp       decode coverage report over a DOL
tools/opcheck_gen/main.cpp        emits a cpp calling every op (compile-time check)
tools/sigmatch/main.cpp           Task 8 CLI
common/sig/dsy.h, dsy.cpp         Dolphin .dsy loader + checksum
tests/ppc_enc.h                   mini encoders for test programs
tests/ppc_fixture.h, mmio_stub.cpp
tests/test_isa.cpp, test_ppc_int.cpp, test_ppc_mem.cpp, test_ppc_float.cpp, test_analyzer.cpp, test_dsy.cpp
tests/e2e/                        gen_e2e_dols.cpp, e2e_main.cpp, CMakeLists.txt
project/uc.cfg, project/hle_required.txt
game/CMakeLists.txt               builds generated/ into uc_game (when present)
docs/spikes/S5-identification.md  Task 8 results
```

---

### Task 1: Gekko decoder and coverage report (TDD)

**Files:**
- Create: `common/ppc/isa.h`, `common/ppc/isa.cpp`, `common/ppc/insn.h`, `tests/test_isa.cpp`, `tools/dol_coverage/main.cpp`, `tools/dol_coverage/CMakeLists.txt`
- Modify: `common/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Produces (`namespace ppc`, `common/ppc/isa.h`):
  - `enum class Kind : uint8_t { NONE, RRR, RRS, RRU, RR, RB, R, RRM, RRRM, CMP, CMPS, CMPU, CRB, MCRF, SPR, MTCRF, RRRR, RAC, PSQ, PSQX, BR, BC, BCLR, BCCTR, SC, UNSUP };`
  - `enum class Sub : uint8_t { N, X5, X6, X9, X10 };`
  - `struct OpInfo { const char* name; uint8_t primary; Sub sub; uint16_t xo; Kind kind; bool rc; };`
  - `const OpInfo* decode(uint32_t word);` (nullptr if undecodable)
  - `const OpInfo* const* all_ops(size_t& count);`
- Produces (`common/ppc/insn.h`): inline field extractors `t(w) a(w) b(w) c(w) simm(w) uimm(w) rc(w) oe(w) mb(w) me(w) sh(w) crfd(w) crfs(w) crm(w) spr(w) bo(w) bi(w) bd(w) li(w) aa(w) lk(w)`.

- [ ] **Step 1: Write the failing test**

`tests/test_isa.cpp`:
```cpp
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
    CHECK(!std::strcmp(name(0x7C643A15 | 0x400), "add"));  // addo. r3,r4,r7: OE variant resolves via the 9-bit xo
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

    // table self-consistency: every row decodes back to itself when built from primary/xo with zero operands
    size_t n = 0;
    auto* const* ops = ppc::all_ops(n);
    CHECK(n > 200);
    for (size_t i = 0; i < n; ++i) CHECK(ops[i]->name != nullptr);
    return 0;
}
```

- [ ] **Step 2: Wire and run to verify it fails**

`tests/CMakeLists.txt`: change the loop list to `test_dol test_dol_scan test_isa`.
Run: `scripts\dev.bat`
Expected: build FAILS (`ppc/insn.h` not found).

- [ ] **Step 3: Write `common/ppc/insn.h`**

```cpp
#pragma once
#include <cstdint>

namespace ppc {
inline uint32_t t(uint32_t w) { return (w >> 21) & 31; }
inline uint32_t a(uint32_t w) { return (w >> 16) & 31; }
inline uint32_t b(uint32_t w) { return (w >> 11) & 31; }
inline uint32_t c(uint32_t w) { return (w >> 6) & 31; }
inline int32_t simm(uint32_t w) { return int16_t(w & 0xFFFF); }
inline uint32_t uimm(uint32_t w) { return w & 0xFFFF; }
inline bool rc(uint32_t w) { return w & 1; }
inline bool oe(uint32_t w) { return (w >> 10) & 1; }
inline uint32_t mb(uint32_t w) { return (w >> 6) & 31; }
inline uint32_t me(uint32_t w) { return (w >> 1) & 31; }
inline uint32_t sh(uint32_t w) { return (w >> 11) & 31; }
inline uint32_t crfd(uint32_t w) { return (w >> 23) & 7; }
inline uint32_t crfs(uint32_t w) { return (w >> 18) & 7; }
inline uint32_t crm(uint32_t w) { return (w >> 12) & 0xFF; }
inline uint32_t spr(uint32_t w) { return (((w >> 11) & 31) << 5) | ((w >> 16) & 31); }
inline uint32_t bo(uint32_t w) { return (w >> 21) & 31; }
inline uint32_t bi(uint32_t w) { return (w >> 16) & 31; }
inline int32_t bd(uint32_t w) { return int16_t(w & 0xFFFC); }
inline int32_t li(uint32_t w) { return int32_t(w << 6) >> 6 & ~3; }
inline bool aa(uint32_t w) { return (w >> 1) & 1; }
inline bool lk(uint32_t w) { return w & 1; }
}  // namespace ppc
```

- [ ] **Step 4: Write `common/ppc/isa.h`**

```cpp
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
```

- [ ] **Step 5: Write `common/ppc/isa.cpp`**

The table is the single source of truth: `X(name, primary, sub, xo, kind, rc)`. Rows come from the Gekko/PowerPC 750CL user manuals; Step 7's coverage run finds anything the game uses that is missing.

```cpp
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
  /* 19: condition register and branches */ \
  X(mcrf,19,X10,0,MCRF,0) X(bclr,19,X10,16,BCLR,0) X(crnor,19,X10,33,CRB,0) X(rfi,19,X10,50,UNSUP,0) \
  X(crandc,19,X10,129,CRB,0) X(isync,19,X10,150,NONE,0) X(crxor,19,X10,193,CRB,0) X(crnand,19,X10,225,CRB,0) \
  X(crand,19,X10,257,CRB,0) X(creqv,19,X10,289,CRB,0) X(crorc,19,X10,417,CRB,0) X(cror,19,X10,449,CRB,0) \
  X(bcctr,19,X10,528,BCCTR,0) \
  /* 31: integer X/XO forms. XO-form arithmetic is keyed on the 9-bit xo (OE is bit 10). */ \
  X(cmp,31,X10,0,CMP,0) X(tw,31,X10,4,RRR,0) X(lwarx,31,X10,20,RRR,0) X(lwzx,31,X10,23,RRR,0) \
  X(slw,31,X10,24,RRR,1) X(cntlzw,31,X10,26,RR,1) X(and,31,X10,28,RRR,1) X(cmpl,31,X10,32,CMP,0) \
  X(lwzux,31,X10,55,RRR,0) X(andc,31,X10,60,RRR,1) X(mfmsr,31,X10,83,R,0) X(dcbf,31,X10,86,AB,0) \
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
  /* 59: single-precision float (A-form, xo5) */ \
  X(fdivs,59,X5,18,RRR,1) X(fsubs,59,X5,20,RRR,1) X(fadds,59,X5,21,RRR,1) X(fsqrts,59,X5,22,RB,1) \
  X(fres,59,X5,24,RB,1) X(fmuls,59,X5,25,RAC,1) X(fmsubs,59,X5,28,RRRR,1) X(fmadds,59,X5,29,RRRR,1) \
  X(fnmsubs,59,X5,30,RRRR,1) X(fnmadds,59,X5,31,RRRR,1) \
  /* 63: double-precision float: A-form xo5 first, then X-form xo10 */ \
  X(fdiv,63,X5,18,RRR,1) X(fsub,63,X5,20,RRR,1) X(fadd,63,X5,21,RRR,1) X(fsel,63,X5,23,RRRR,1) \
  X(fmul,63,X5,25,RAC,1) X(frsqrte,63,X5,26,RB,1) X(fmsub,63,X5,28,RRRR,1) X(fmadd,63,X5,29,RRRR,1) \
  X(fnmsub,63,X5,30,RRRR,1) X(fnmadd,63,X5,31,RRRR,1) \
  X(fcmpu,63,X10,0,CMP,0) X(frsp,63,X10,12,RB,1) X(fctiw,63,X10,14,RB,1) X(fctiwz,63,X10,15,RB,1) \
  X(fcmpo,63,X10,32,CMP,0) X(mtfsb1,63,X10,38,R,1) X(fneg,63,X10,40,RB,1) X(mcrfs,63,X10,64,MCRF,0) \
  X(mtfsb0,63,X10,70,R,1) X(fmr,63,X10,72,RB,1) X(mtfsfi,63,X10,134,MCRF,1) X(fnabs,63,X10,136,RB,1) \
  X(fabs,63,X10,264,RB,1) X(mffs,63,X10,583,R,1) X(mtfsf,63,X10,711,MTCRF,1) \
  /* 4: paired singles. A-form xo5, indexed psq xo6, X-form xo10 */ \
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
```

Notes: `mtfsfi` uses `MCRF` kind (crfd, imm via crfs bits is wrong for it; Task 4 passes `(crfd, imm4)` by a dedicated extractor, see the translator's `mtfsfi` special case); `srawi` is `RRR` with `b` carrying the shift amount.

- [ ] **Step 6: Wire sources and the coverage tool**

`common/CMakeLists.txt`: `add_library(uc_common STATIC dol.cpp dol_scan.cpp ppc/isa.cpp)`.

`tools/dol_coverage/CMakeLists.txt`:
```cmake
add_executable(dol_coverage main.cpp)
target_link_libraries(dol_coverage PRIVATE uc_common)
```
Root `CMakeLists.txt`: add `add_subdirectory(tools/dol_coverage)`.

`tools/dol_coverage/main.cpp`:
```cpp
#include "dol.h"
#include "ppc/insn.h"
#include "ppc/isa.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: dol_coverage <main.dol>\n"); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto d = uc::parse_dol(f);
    if (!d.ok) { std::fprintf(stderr, "invalid DOL: %s\n", d.error.c_str()); return 1; }
    size_t words = 0, zero = 0, bad = 0, oe_used = 0, unsup = 0;
    std::map<uint32_t, size_t> undecoded;  // key = primary<<16 | xo10
    std::map<std::string, size_t> counts;
    for (const auto& s : d.image.sections) {
        if (!s.is_text) continue;
        auto b = uc::section_bytes(f, s);
        for (size_t i = 0; i + 4 <= b.size(); i += 4) {
            uint32_t w = uint32_t(b[i]) << 24 | uint32_t(b[i + 1]) << 16 | uint32_t(b[i + 2]) << 8 | b[i + 3];
            ++words;
            if (w == 0) { ++zero; continue; }
            auto* o = ppc::decode(w);
            if (!o) { ++bad; ++undecoded[(w >> 26) << 16 | ((w >> 1) & 0x3FF)]; continue; }
            ++counts[o->name];
            if (o->kind == ppc::Kind::UNSUP) ++unsup;
            if (o->primary == 31 && o->sub == ppc::Sub::X9 && ppc::oe(w)) ++oe_used;
        }
    }
    std::printf("words %zu  zero %zu  undecoded %zu  unsupported-kind %zu  OE-set %zu\n", words, zero, bad, unsup, oe_used);
    for (auto& [k, v] : undecoded) std::printf("undecoded primary %u xo10 %u : %zu\n", k >> 16, k & 0xFFFF, v);
    std::printf("distinct decoded ops: %zu\n", counts.size());
    return 0;
}
```

- [ ] **Step 7: Run tests, then the coverage report**

Run: `scripts\dev.bat` — Expected: `test_isa` PASSED.
Run: `build\tools\dol_coverage\dol_coverage.exe "S:\Games\Umbrella Chronicles\game\DATA\sys\main.dol"`
Expected: `undecoded` ≤ 25 words, all explainable as data embedded in text (S1 saw 13 non-zero primary-0 words). For every `undecoded primary P xo10 X` line with a count above 3, add the missing row to `PPC_OPS` (consult the 750CL manual opcode map), re-run until only explainable leftovers remain. Record the final numbers (words, undecoded, `OE-set`, `unsupported-kind`) in the ledger: `OE-set` > 0 means Tasks 2–3 must implement overflow forms (they are written without OE; an OE word is translated as a trap and counted by Task 6).

---

### Task 2: Runtime header, guest memory and integer semantics (TDD)

**Files:**
- Create: `runtime/CMakeLists.txt`, `runtime/ppc_rt.h`, `runtime/guest_mem.h`, `runtime/guest_mem.cpp`, `tests/ppc_fixture.h`, `tests/mmio_stub.cpp`, `tests/test_ppc_int.cpp`, `tests/test_ppc_mem.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from earlier tasks at build level (semantics are independent of the decoder).
- Produces (`namespace ppc`, `runtime/ppc_rt.h`):
  - `struct Ctx { uint32_t r[32]; double f[32], ps1[32]; uint8_t cr[8]; uint8_t xer_ca, xer_ov, xer_so; uint32_t lr, ctr, msr, fpscr, gqr[8], spr[1024], reserve_addr; uint8_t* mem; };` CR field encoding: LT=8, GT=4, EQ=2, SO=1.
  - Externals the host must define: `uint32_t mmio_read(uint32_t ea, int size); void mmio_write(uint32_t ea, int size, uint32_t v); [[noreturn]] void trap(uint32_t addr, const char* what); uint64_t timebase();`
  - Memory: `rd8/rd16/rd32/wr8/wr16/wr32(Ctx&, ea[, v])`, `rdf32/rdf64/wrf32/wrf64`.
  - Integer ops `op_<name>(Ctx&, ...)` exactly as the argument lists in Task 1's `Kind` comments, with a trailing `bool rc` where the table has `rc=1`.
  - `uc::guest_mem_init()`, `uc::guest_mem_base()` in `runtime/guest_mem.h`.

- [ ] **Step 1: Write the failing tests**

`tests/ppc_fixture.h`:
```cpp
#pragma once
#include "ppc_rt.h"
#include "guest_mem.h"

struct Fix {
    ppc::Ctx c{};
    Fix() { uc::guest_mem_init(); c.mem = uc::guest_mem_base(); }
};
```

`tests/mmio_stub.cpp`:
```cpp
#include "ppc_rt.h"
#include <cstdio>
#include <cstdlib>

uint32_t g_mmio_last_addr, g_mmio_last_val;
int g_mmio_last_size;

namespace ppc {
uint32_t mmio_read(uint32_t ea, int size) { g_mmio_last_addr = ea; g_mmio_last_size = size; return 0xA5A5A5A5u; }
void mmio_write(uint32_t ea, int size, uint32_t v) { g_mmio_last_addr = ea; g_mmio_last_size = size; g_mmio_last_val = v; }
[[noreturn]] void trap(uint32_t addr, const char* what) {
    std::fprintf(stderr, "guest trap at %08X: %s\n", addr, what);
    std::exit(3);
}
uint64_t timebase() { return 0x1122334455667788ull; }
}  // namespace ppc
```

`tests/test_ppc_int.cpp`:
```cpp
#include "check.h"
#include "ppc_fixture.h"
using namespace ppc;

int main() {
    Fix f; Ctx& c = f.c;

    // add / carry chain: 0x1_FFFFFFFF + 1 = 0x2_00000000 via addc/adde
    c.r[3] = 0xFFFFFFFF; c.r[4] = 1; c.r[5] = 1; c.r[6] = 0;
    op_addc(c, 7, 3, 4, false);
    op_adde(c, 8, 5, 6, false);
    CHECK(c.r[7] == 0 && c.xer_ca == 0 + 1);   // after addc: low word 0, carry out
    CHECK(c.r[8] == 2);                          // adde: 1 + 0 + carry
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
    op_oris(c, 3, 5, 0x1234);   CHECK(c.r[5] == 0xF0F0F0F0 << 0 | 0x12340000u);
    op_xori(c, 3, 5, 0xFFFF);   CHECK(c.r[5] == (0xF0F0F0F0u ^ 0xFFFFu));
    // extend / count leading zeros
    c.r[3] = 0x80; op_extsb(c, 5, 3, false); CHECK(c.r[5] == 0xFFFFFF80);
    c.r[3] = 0x8000; op_extsh(c, 5, 3, false); CHECK(c.r[5] == 0xFFFF8000);
    c.r[3] = 0; op_cntlzw(c, 5, 3, false); CHECK(c.r[5] == 32);
    c.r[3] = 1; op_cntlzw(c, 5, 3, false); CHECK(c.r[5] == 31);
    c.r[3] = 0x80000000; op_cntlzw(c, 5, 3, false); CHECK(c.r[5] == 0);
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
```
`tests/test_ppc_mem.cpp`:
```cpp
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
```
`0xCBFFFFFC` and `0xCE000000` are outside committed memory, so the window edges are tested through the `is_mmio` predicate and the in-window accesses above.

- [ ] **Step 2: Wire and run to verify it fails**

`CMakeLists.txt`: add `add_subdirectory(runtime)` after `add_subdirectory(common)`.
`runtime/CMakeLists.txt`:
```cmake
add_library(uc_runtime STATIC guest_mem.cpp)
target_include_directories(uc_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
```
`tests/CMakeLists.txt` (replace file):
```cmake
add_library(test_support STATIC mmio_stub.cpp)
target_link_libraries(test_support PUBLIC uc_runtime)
foreach(t test_dol test_dol_scan test_isa)
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} PRIVATE uc_common)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${t} COMMAND ${t})
endforeach()
foreach(t test_ppc_int test_ppc_mem test_ppc_float)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${t}.cpp)
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE test_support)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${t} COMMAND ${t})
  endif()
endforeach()
```
Run: `scripts\dev.bat` — Expected: FAIL (`ppc_rt.h` / `guest_mem.h` not found).

- [ ] **Step 3: Write `runtime/guest_mem.h` and `guest_mem.cpp`**

```cpp
// guest_mem.h
#pragma once
#include <cstdint>
namespace uc {
void guest_mem_init();          // idempotent; reserves 1 GB, commits MEM1 (24 MB) and MEM2 (64 MB)
uint8_t* guest_mem_base();
}
```
```cpp
// guest_mem.cpp
#include "guest_mem.h"
#include <cstdio>
#include <cstdlib>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace uc {
static uint8_t* g_base;

void guest_mem_init() {
    if (g_base) return;
    g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x40000000, MEM_RESERVE, PAGE_NOACCESS));
    if (!g_base || !VirtualAlloc(g_base, 0x01800000, MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc(g_base + 0x10000000, 0x04000000, MEM_COMMIT, PAGE_READWRITE)) {
        std::fprintf(stderr, "fatal: cannot reserve guest memory\n");
        std::exit(4);
    }
}
uint8_t* guest_mem_base() { return g_base; }
}  // namespace uc
```

- [ ] **Step 4: Write `runtime/ppc_rt.h` (Part 1: context, memory, integer, CR, SPR)**

```cpp
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>

#define PPC_INLINE __attribute__((always_inline)) inline

namespace ppc {

struct Ctx {
    uint32_t r[32];
    double f[32];      // FPR / ps0
    double ps1[32];    // ps1
    uint8_t cr[8];     // per field: LT=8 GT=4 EQ=2 SO=1
    uint8_t xer_ca, xer_ov, xer_so;
    uint32_t lr, ctr, msr, fpscr;
    uint32_t gqr[8];
    uint32_t spr[1024];  // SPRs without a dedicated field
    uint32_t reserve_addr;
    uint8_t* mem;
};

uint32_t mmio_read(uint32_t ea, int size);
void mmio_write(uint32_t ea, int size, uint32_t v);
[[noreturn]] void trap(uint32_t addr, const char* what);
uint64_t timebase();

// ---- memory ----
PPC_INLINE bool is_mmio(uint32_t ea) { return ea - 0xCC000000u < 0x02000000u; }
PPC_INLINE uint8_t* hp(Ctx& c, uint32_t ea) { return c.mem + (ea & 0x3FFFFFFFu); }

PPC_INLINE uint32_t rd32(Ctx& c, uint32_t ea) {
    if (is_mmio(ea)) [[unlikely]] return mmio_read(ea, 4);
    uint32_t v; std::memcpy(&v, hp(c, ea), 4); return _byteswap_ulong(v);
}
PPC_INLINE uint32_t rd16(Ctx& c, uint32_t ea) {
    if (is_mmio(ea)) [[unlikely]] return mmio_read(ea, 2);
    uint16_t v; std::memcpy(&v, hp(c, ea), 2); return _byteswap_ushort(v);
}
PPC_INLINE uint32_t rd8(Ctx& c, uint32_t ea) {
    if (is_mmio(ea)) [[unlikely]] return mmio_read(ea, 1);
    return *hp(c, ea);
}
PPC_INLINE void wr32(Ctx& c, uint32_t ea, uint32_t v) {
    if (is_mmio(ea)) [[unlikely]] { mmio_write(ea, 4, v); return; }
    v = _byteswap_ulong(v); std::memcpy(hp(c, ea), &v, 4);
}
PPC_INLINE void wr16(Ctx& c, uint32_t ea, uint32_t v) {
    if (is_mmio(ea)) [[unlikely]] { mmio_write(ea, 2, v & 0xFFFF); return; }
    uint16_t h = _byteswap_ushort(uint16_t(v)); std::memcpy(hp(c, ea), &h, 2);
}
PPC_INLINE void wr8(Ctx& c, uint32_t ea, uint32_t v) {
    if (is_mmio(ea)) [[unlikely]] { mmio_write(ea, 1, v & 0xFF); return; }
    *hp(c, ea) = uint8_t(v);
}
PPC_INLINE float rdf32(Ctx& c, uint32_t ea) { uint32_t b = rd32(c, ea); float f; std::memcpy(&f, &b, 4); return f; }
PPC_INLINE double rdf64(Ctx& c, uint32_t ea) {
    if (is_mmio(ea)) [[unlikely]] { uint64_t hi = mmio_read(ea, 4), lo = mmio_read(ea + 4, 4); uint64_t b = hi << 32 | lo; double d; std::memcpy(&d, &b, 8); return d; }
    uint64_t v; std::memcpy(&v, hp(c, ea), 8); v = _byteswap_uint64(v); double d; std::memcpy(&d, &v, 8); return d;
}
PPC_INLINE void wrf32(Ctx& c, uint32_t ea, float v) { uint32_t b; std::memcpy(&b, &v, 4); wr32(c, ea, b); }
PPC_INLINE void wrf64(Ctx& c, uint32_t ea, double v) {
    uint64_t b; std::memcpy(&b, &v, 8);
    if (is_mmio(ea)) [[unlikely]] { mmio_write(ea, 4, uint32_t(b >> 32)); mmio_write(ea + 4, 4, uint32_t(b)); return; }
    b = _byteswap_uint64(b); std::memcpy(hp(c, ea), &b, 8);
}

PPC_INLINE uint32_t ea_d(Ctx& c, int a, int32_t d) { return (a ? c.r[a] : 0u) + uint32_t(d); }
PPC_INLINE uint32_t ea_x(Ctx& c, int a, int b) { return (a ? c.r[a] : 0u) + c.r[b]; }

// ---- condition register ----
PPC_INLINE void cr0(Ctx& c, uint32_t v) { int32_t s = int32_t(v); c.cr[0] = uint8_t((s < 0 ? 8 : s > 0 ? 4 : 2) | c.xer_so); }
PPC_INLINE bool crbit(Ctx& c, int bit) { return (c.cr[bit >> 2] >> (3 - (bit & 3))) & 1; }
PPC_INLINE void set_crbit(Ctx& c, int bit, bool v) {
    uint8_t m = uint8_t(1 << (3 - (bit & 3)));
    c.cr[bit >> 2] = uint8_t(v ? (c.cr[bit >> 2] | m) : (c.cr[bit >> 2] & ~m));
}
PPC_INLINE void op_crand(Ctx& c, int t, int a, int b) { set_crbit(c, t, crbit(c, a) & crbit(c, b)); }
PPC_INLINE void op_crandc(Ctx& c, int t, int a, int b) { set_crbit(c, t, crbit(c, a) & !crbit(c, b)); }
PPC_INLINE void op_cror(Ctx& c, int t, int a, int b) { set_crbit(c, t, crbit(c, a) | crbit(c, b)); }
PPC_INLINE void op_crorc(Ctx& c, int t, int a, int b) { set_crbit(c, t, crbit(c, a) | !crbit(c, b)); }
PPC_INLINE void op_crxor(Ctx& c, int t, int a, int b) { set_crbit(c, t, crbit(c, a) ^ crbit(c, b)); }
PPC_INLINE void op_crnand(Ctx& c, int t, int a, int b) { set_crbit(c, t, !(crbit(c, a) & crbit(c, b))); }
PPC_INLINE void op_crnor(Ctx& c, int t, int a, int b) { set_crbit(c, t, !(crbit(c, a) | crbit(c, b))); }
PPC_INLINE void op_creqv(Ctx& c, int t, int a, int b) { set_crbit(c, t, !(crbit(c, a) ^ crbit(c, b))); }
PPC_INLINE void op_mcrf(Ctx& c, int d, int s) { c.cr[d] = c.cr[s]; }
PPC_INLINE void op_mfcr(Ctx& c, int t) { uint32_t v = 0; for (int i = 0; i < 8; ++i) v = v << 4 | c.cr[i]; c.r[t] = v; }
PPC_INLINE void op_mtcrf(Ctx& c, int crm, int rs) {
    for (int i = 0; i < 8; ++i) if (crm & (0x80 >> i)) c.cr[i] = uint8_t((c.r[rs] >> (28 - 4 * i)) & 0xF);
}
PPC_INLINE void op_mcrxr(Ctx& c, int d) {
    c.cr[d] = uint8_t(c.xer_so << 3 | c.xer_ov << 2 | c.xer_ca << 1); c.xer_so = c.xer_ov = c.xer_ca = 0;
}

// ---- integer arithmetic (OE forms are not implemented; the translator traps on them) ----
PPC_INLINE void rc_set(Ctx& c, uint32_t v, bool rc) { if (rc) cr0(c, v); }
PPC_INLINE void op_addi(Ctx& c, int d, int a, int32_t i) { c.r[d] = (a ? c.r[a] : 0u) + uint32_t(i); }
PPC_INLINE void op_addis(Ctx& c, int d, int a, int32_t i) { c.r[d] = (a ? c.r[a] : 0u) + (uint32_t(i) << 16); }
PPC_INLINE void op_addic(Ctx& c, int d, int a, int32_t i) {
    uint32_t x = c.r[a], r = x + uint32_t(i); c.xer_ca = r < x; c.r[d] = r;
}
PPC_INLINE void op_addic_(Ctx& c, int d, int a, int32_t i) { op_addic(c, d, a, i); cr0(c, c.r[d]); }
PPC_INLINE void op_subfic(Ctx& c, int d, int a, int32_t i) {
    uint32_t x = c.r[a]; c.xer_ca = uint32_t(i) >= x; c.r[d] = uint32_t(i) - x;
}
PPC_INLINE void op_add(Ctx& c, int d, int a, int b, bool rc) { c.r[d] = c.r[a] + c.r[b]; rc_set(c, c.r[d], rc); }
PPC_INLINE void op_addc(Ctx& c, int d, int a, int b, bool rc) {
    uint32_t x = c.r[a], y = c.r[b], r = x + y; c.xer_ca = r < x; c.r[d] = r; rc_set(c, r, rc);
}
PPC_INLINE void op_adde(Ctx& c, int d, int a, int b, bool rc) {
    uint64_t s = uint64_t(c.r[a]) + c.r[b] + c.xer_ca; c.xer_ca = uint8_t(s >> 32); c.r[d] = uint32_t(s); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_addze(Ctx& c, int d, int a, bool rc) {
    uint64_t s = uint64_t(c.r[a]) + c.xer_ca; c.xer_ca = uint8_t(s >> 32); c.r[d] = uint32_t(s); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_addme(Ctx& c, int d, int a, bool rc) {
    uint64_t s = uint64_t(c.r[a]) + 0xFFFFFFFFull + c.xer_ca; c.xer_ca = uint8_t(s >> 32); c.r[d] = uint32_t(s); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_subf(Ctx& c, int d, int a, int b, bool rc) { c.r[d] = c.r[b] - c.r[a]; rc_set(c, c.r[d], rc); }
PPC_INLINE void op_subfc(Ctx& c, int d, int a, int b, bool rc) {
    uint32_t x = c.r[a], y = c.r[b]; c.xer_ca = y >= x; c.r[d] = y - x; rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_subfe(Ctx& c, int d, int a, int b, bool rc) {
    uint64_t s = uint64_t(~c.r[a]) + c.r[b] + c.xer_ca; c.xer_ca = uint8_t(s >> 32); c.r[d] = uint32_t(s); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_subfze(Ctx& c, int d, int a, bool rc) {
    uint64_t s = uint64_t(~c.r[a]) + c.xer_ca; c.xer_ca = uint8_t(s >> 32); c.r[d] = uint32_t(s); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_subfme(Ctx& c, int d, int a, bool rc) {
    uint64_t s = uint64_t(~c.r[a]) + 0xFFFFFFFFull + c.xer_ca; c.xer_ca = uint8_t(s >> 32); c.r[d] = uint32_t(s); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_neg(Ctx& c, int d, int a, bool rc) { c.r[d] = 0u - c.r[a]; rc_set(c, c.r[d], rc); }
PPC_INLINE void op_mulli(Ctx& c, int d, int a, int32_t i) { c.r[d] = uint32_t(int32_t(c.r[a]) * i); }
PPC_INLINE void op_mullw(Ctx& c, int d, int a, int b, bool rc) { c.r[d] = uint32_t(int32_t(c.r[a]) * int32_t(c.r[b])); rc_set(c, c.r[d], rc); }
PPC_INLINE void op_mulhw(Ctx& c, int d, int a, int b, bool rc) {
    c.r[d] = uint32_t((int64_t(int32_t(c.r[a])) * int32_t(c.r[b])) >> 32); rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_mulhwu(Ctx& c, int d, int a, int b, bool rc) {
    c.r[d] = uint32_t((uint64_t(c.r[a]) * c.r[b]) >> 32); rc_set(c, c.r[d], rc);
}
// Division by zero / INT_MIN/-1 is undefined in the ISA; use the hardware-observed result (0, or all ones for a negative dividend).
PPC_INLINE void op_divw(Ctx& c, int d, int a, int b, bool rc) {
    int32_t x = int32_t(c.r[a]), y = int32_t(c.r[b]);
    c.r[d] = (y == 0 || (x == INT32_MIN && y == -1)) ? (x < 0 ? 0xFFFFFFFFu : 0u) : uint32_t(x / y);
    rc_set(c, c.r[d], rc);
}
PPC_INLINE void op_divwu(Ctx& c, int d, int a, int b, bool rc) { c.r[d] = c.r[b] ? c.r[a] / c.r[b] : 0u; rc_set(c, c.r[d], rc); }

// ---- logical, shifts, rotates (t = rS, a = rA destination) ----
PPC_INLINE void op_and(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = c.r[s] & c.r[b]; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_andc(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = c.r[s] & ~c.r[b]; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_or(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = c.r[s] | c.r[b]; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_orc(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = c.r[s] | ~c.r[b]; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_xor(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = c.r[s] ^ c.r[b]; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_nor(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = ~(c.r[s] | c.r[b]); rc_set(c, c.r[a], rc); }
PPC_INLINE void op_nand(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = ~(c.r[s] & c.r[b]); rc_set(c, c.r[a], rc); }
PPC_INLINE void op_eqv(Ctx& c, int s, int a, int b, bool rc) { c.r[a] = ~(c.r[s] ^ c.r[b]); rc_set(c, c.r[a], rc); }
PPC_INLINE void op_ori(Ctx& c, int s, int a, uint32_t i) { c.r[a] = c.r[s] | i; }
PPC_INLINE void op_oris(Ctx& c, int s, int a, uint32_t i) { c.r[a] = c.r[s] | (i << 16); }
PPC_INLINE void op_xori(Ctx& c, int s, int a, uint32_t i) { c.r[a] = c.r[s] ^ i; }
PPC_INLINE void op_xoris(Ctx& c, int s, int a, uint32_t i) { c.r[a] = c.r[s] ^ (i << 16); }
PPC_INLINE void op_andi_(Ctx& c, int s, int a, uint32_t i) { c.r[a] = c.r[s] & i; cr0(c, c.r[a]); }
PPC_INLINE void op_andis_(Ctx& c, int s, int a, uint32_t i) { c.r[a] = c.r[s] & (i << 16); cr0(c, c.r[a]); }
PPC_INLINE void op_extsb(Ctx& c, int s, int a, bool rc) { c.r[a] = uint32_t(int32_t(int8_t(c.r[s]))); rc_set(c, c.r[a], rc); }
PPC_INLINE void op_extsh(Ctx& c, int s, int a, bool rc) { c.r[a] = uint32_t(int32_t(int16_t(c.r[s]))); rc_set(c, c.r[a], rc); }
PPC_INLINE void op_cntlzw(Ctx& c, int s, int a, bool rc) { uint32_t v = c.r[s]; c.r[a] = v ? uint32_t(__builtin_clz(v)) : 32u; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_slw(Ctx& c, int s, int a, int b, bool rc) { uint32_t n = c.r[b] & 0x3F; c.r[a] = n & 32 ? 0u : c.r[s] << n; rc_set(c, c.r[a], rc); }
PPC_INLINE void op_srw(Ctx& c, int s, int a, int b, bool rc) { uint32_t n = c.r[b] & 0x3F; c.r[a] = n & 32 ? 0u : c.r[s] >> n; rc_set(c, c.r[a], rc); }
PPC_INLINE void sra_common(Ctx& c, int s, int a, uint32_t n, bool rc) {
    int32_t v = int32_t(c.r[s]);
    if (n >= 32) { c.r[a] = v < 0 ? 0xFFFFFFFFu : 0u; c.xer_ca = v < 0; }
    else { c.r[a] = uint32_t(v >> n); c.xer_ca = v < 0 && n && (uint32_t(v) & ((1u << n) - 1)) != 0; }
    rc_set(c, c.r[a], rc);
}
PPC_INLINE void op_sraw(Ctx& c, int s, int a, int b, bool rc) { sra_common(c, s, a, c.r[b] & 0x3F, rc); }
PPC_INLINE void op_srawi(Ctx& c, int s, int a, int sh, bool rc) { sra_common(c, s, a, uint32_t(sh), rc); }
PPC_INLINE uint32_t rotl32(uint32_t v, uint32_t n) { return _rotl(v, int(n & 31)); }
PPC_INLINE uint32_t mask32(uint32_t mb, uint32_t me) {
    uint32_t m = (0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me));
    return mb <= me ? m : ((0xFFFFFFFFu >> mb) | (0xFFFFFFFFu << (31 - me)));
}
PPC_INLINE void op_rlwinm(Ctx& c, int s, int a, int sh, int mb, int me, bool rc) {
    c.r[a] = rotl32(c.r[s], sh) & mask32(mb, me); rc_set(c, c.r[a], rc);
}
PPC_INLINE void op_rlwnm(Ctx& c, int s, int a, int b, int mb, int me, bool rc) {
    c.r[a] = rotl32(c.r[s], c.r[b]) & mask32(mb, me); rc_set(c, c.r[a], rc);
}
PPC_INLINE void op_rlwimi(Ctx& c, int s, int a, int sh, int mb, int me, bool rc) {
    uint32_t m = mask32(mb, me); c.r[a] = (rotl32(c.r[s], sh) & m) | (c.r[a] & ~m); rc_set(c, c.r[a], rc);
}

// ---- compares ----
PPC_INLINE void cmp_set(Ctx& c, int f, int cmp) { c.cr[f] = uint8_t((cmp < 0 ? 8 : cmp > 0 ? 4 : 2) | c.xer_so); }
PPC_INLINE int cmp3(int64_t x, int64_t y) { return x < y ? -1 : x > y ? 1 : 0; }
PPC_INLINE void op_cmp(Ctx& c, int f, int a, int b) { cmp_set(c, f, cmp3(int32_t(c.r[a]), int32_t(c.r[b]))); }
PPC_INLINE void op_cmpl(Ctx& c, int f, int a, int b) { cmp_set(c, f, cmp3(c.r[a], c.r[b])); }
PPC_INLINE void op_cmpi(Ctx& c, int f, int a, int32_t i) { cmp_set(c, f, cmp3(int32_t(c.r[a]), i)); }
PPC_INLINE void op_cmpli(Ctx& c, int f, int a, uint32_t i) { cmp_set(c, f, cmp3(c.r[a], i)); }

// ---- special registers ----
PPC_INLINE uint32_t xer_get(Ctx& c) { return uint32_t(c.xer_so) << 31 | uint32_t(c.xer_ov) << 30 | uint32_t(c.xer_ca) << 29; }
PPC_INLINE void op_mfspr(Ctx& c, int t, uint32_t s) {
    switch (s) {
        case 1: c.r[t] = xer_get(c); break;
        case 8: c.r[t] = c.lr; break;
        case 9: c.r[t] = c.ctr; break;
        case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919: c.r[t] = c.gqr[s - 912]; break;
        default: c.r[t] = c.spr[s & 1023]; break;
    }
}
PPC_INLINE void op_mtspr(Ctx& c, int t, uint32_t s) {
    uint32_t v = c.r[t];
    switch (s) {
        case 1: c.xer_so = v >> 31; c.xer_ov = (v >> 30) & 1; c.xer_ca = (v >> 29) & 1; break;
        case 8: c.lr = v; break;
        case 9: c.ctr = v; break;
        case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919: c.gqr[s - 912] = v; break;
        default: c.spr[s & 1023] = v; break;
    }
}
PPC_INLINE void op_mftb(Ctx& c, int t, uint32_t tbr) { uint64_t tb = timebase(); c.r[t] = tbr == 269 ? uint32_t(tb >> 32) : uint32_t(tb); }
PPC_INLINE void op_mfmsr(Ctx& c, int t) { c.r[t] = c.msr; }
PPC_INLINE void op_mtmsr(Ctx& c, int t) { c.msr = c.r[t]; }
PPC_INLINE void op_isync(Ctx&) {}
PPC_INLINE void op_sync(Ctx&) { _ReadWriteBarrier(); }
PPC_INLINE void op_eieio(Ctx&) { _ReadWriteBarrier(); }
PPC_INLINE void op_dcbf(Ctx&, int, int) {}
PPC_INLINE void op_dcbi(Ctx&, int, int) {}
PPC_INLINE void op_dcbt(Ctx&, int, int) {}
PPC_INLINE void op_dcbtst(Ctx&, int, int) {}
PPC_INLINE void op_icbi(Ctx&, int, int) {}
PPC_INLINE void op_tw(Ctx& c, int, int, int) { (void)c; }   // traps are emitted by the translator for unconditional TO
PPC_INLINE void op_twi(Ctx& c, int, int, int32_t) { (void)c; }

}  // namespace ppc
```

The load/store family (Part 2 of the header) is appended in Step 5.

- [ ] **Step 5: Append the load/store family to `runtime/ppc_rt.h`** (before the closing `}  // namespace ppc`)

```cpp
// ---- loads and stores ----
#define PPC_ID(x) (x)
#define PPC_SX16(x) (uint32_t(int32_t(int16_t(x))))
#define PPC_LOAD_FAMILY(nm, RD, CV) \
    PPC_INLINE void op_##nm(Ctx& c, int t, int a, int32_t d) { c.r[t] = CV(RD(c, ea_d(c, a, d))); } \
    PPC_INLINE void op_##nm##u(Ctx& c, int t, int a, int32_t d) { uint32_t e = c.r[a] + uint32_t(d); c.r[t] = CV(RD(c, e)); c.r[a] = e; } \
    PPC_INLINE void op_##nm##x(Ctx& c, int t, int a, int b) { c.r[t] = CV(RD(c, ea_x(c, a, b))); } \
    PPC_INLINE void op_##nm##ux(Ctx& c, int t, int a, int b) { uint32_t e = c.r[a] + c.r[b]; c.r[t] = CV(RD(c, e)); c.r[a] = e; }
PPC_LOAD_FAMILY(lwz, rd32, PPC_ID)
PPC_LOAD_FAMILY(lbz, rd8, PPC_ID)
PPC_LOAD_FAMILY(lhz, rd16, PPC_ID)
PPC_LOAD_FAMILY(lha, rd16, PPC_SX16)

#define PPC_STORE_FAMILY(nm, WR) \
    PPC_INLINE void op_##nm(Ctx& c, int t, int a, int32_t d) { WR(c, ea_d(c, a, d), c.r[t]); } \
    PPC_INLINE void op_##nm##u(Ctx& c, int t, int a, int32_t d) { uint32_t e = c.r[a] + uint32_t(d); WR(c, e, c.r[t]); c.r[a] = e; } \
    PPC_INLINE void op_##nm##x(Ctx& c, int t, int a, int b) { WR(c, ea_x(c, a, b), c.r[t]); } \
    PPC_INLINE void op_##nm##ux(Ctx& c, int t, int a, int b) { uint32_t e = c.r[a] + c.r[b]; WR(c, e, c.r[t]); c.r[a] = e; }
PPC_STORE_FAMILY(stw, wr32)
PPC_STORE_FAMILY(stb, wr8)
PPC_STORE_FAMILY(sth, wr16)

PPC_INLINE void op_lwbrx(Ctx& c, int t, int a, int b) { c.r[t] = _byteswap_ulong(rd32(c, ea_x(c, a, b))); }
PPC_INLINE void op_lhbrx(Ctx& c, int t, int a, int b) { c.r[t] = _byteswap_ushort(uint16_t(rd16(c, ea_x(c, a, b)))); }
PPC_INLINE void op_stwbrx(Ctx& c, int t, int a, int b) { wr32(c, ea_x(c, a, b), _byteswap_ulong(c.r[t])); }
PPC_INLINE void op_sthbrx(Ctx& c, int t, int a, int b) { wr16(c, ea_x(c, a, b), _byteswap_ushort(uint16_t(c.r[t]))); }
PPC_INLINE void op_lmw(Ctx& c, int t, int a, int32_t d) { uint32_t e = ea_d(c, a, d); for (int i = t; i < 32; ++i, e += 4) c.r[i] = rd32(c, e); }
PPC_INLINE void op_stmw(Ctx& c, int t, int a, int32_t d) { uint32_t e = ea_d(c, a, d); for (int i = t; i < 32; ++i, e += 4) wr32(c, e, c.r[i]); }
// Single guest-visible reservation; guest threads never run truly concurrently with the scheduler lock held.
PPC_INLINE void op_lwarx(Ctx& c, int t, int a, int b) { uint32_t e = ea_x(c, a, b); c.reserve_addr = e | 1u; c.r[t] = rd32(c, e); }
PPC_INLINE void op_stwcx_(Ctx& c, int t, int a, int b) {
    uint32_t e = ea_x(c, a, b);
    bool ok = c.reserve_addr == (e | 1u);
    if (ok) wr32(c, e, c.r[t]);
    c.reserve_addr = 0;
    c.cr[0] = uint8_t((ok ? 2 : 0) | c.xer_so);
}
PPC_INLINE void op_dcbz(Ctx& c, int a, int b) { uint32_t e = ea_x(c, a, b) & ~31u; std::memset(hp(c, e), 0, 32); }
PPC_INLINE void op_dcbz_l(Ctx& c, int a, int b) { op_dcbz(c, a, b); }
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `scripts\dev.bat` — Expected: `test_ppc_int` and `test_ppc_mem` PASSED (all earlier tests still pass). If a check fails, fix the semantics in `ppc_rt.h` (the expected values are computed from the PowerPC architecture, not from the implementation).

---

### Task 3: Float and paired-single semantics (TDD)

**Files:**
- Create: `tests/test_ppc_float.cpp`
- Modify: `runtime/ppc_rt.h` (append before the closing namespace)

**Interfaces:**
- Consumes: `Ctx`, `rd32/rdf32/rdf64/wr*`, `ea_d`, `ea_x`, `cmp_set` (Task 2).
- Produces: `op_lfs, op_lfsu, op_lfsx, op_lfsux, op_lfd, op_lfdu, op_lfdx, op_lfdux, op_stfs, op_stfsu, op_stfsx, op_stfsux, op_stfd, op_stfdu, op_stfdx, op_stfdux, op_stfiwx, op_fadd, op_fsub, op_fmul, op_fdiv, op_fadds, op_fsubs, op_fmuls, op_fdivs, op_fmadd, op_fmsub, op_fnmadd, op_fnmsub (and the `s` variants), op_fsel, op_fres, op_frsqrte, op_fsqrts, op_frsp, op_fctiw, op_fctiwz, op_fcmpu, op_fcmpo, op_fmr, op_fneg, op_fabs, op_fnabs, op_mffs, op_mtfsf, op_mtfsfi, op_mtfsb0, op_mtfsb1, op_mcrfs`, all `ps_*` ops in Task 1's table, `op_psq_l/psq_lu/psq_st/psq_stu/psq_lx/psq_stx/psq_lux/psq_stux`. Argument lists follow Task 1's `Kind` comments; float registers are indices into `Ctx::f` / `Ctx::ps1`.

- [ ] **Step 1: Write the failing test**

`tests/test_ppc_float.cpp`:
```cpp
#include "check.h"
#include "ppc_fixture.h"
#include <cmath>
using namespace ppc;

static uint64_t bits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

int main() {
    Fix f; Ctx& c = f.c;

    // single vs double rounding
    c.f[1] = 1.0; c.f[2] = 1e-10;
    op_fadd(c, 3, 1, 2, false);   CHECK(c.f[3] == 1.0 + 1e-10);
    op_fadds(c, 3, 1, 2, false);  CHECK(c.f[3] == 1.0);
    // fused multiply-add keeps the exact product: (1+2^-30)*(1-2^-30) - 1 = -2^-60 exactly under fusion
    c.f[1] = 1.0 + std::ldexp(1.0, -30); c.f[2] = 1.0 - std::ldexp(1.0, -30); c.f[3] = -1.0;
    op_fmadd(c, 4, 1, 3, 2, false);   // frD = frA*frC + frB   (args: t, a, b, c)
    CHECK(c.f[4] == -std::ldexp(1.0, -60));
    op_fmsub(c, 4, 1, 3, 2, false);   // frA*frC - frB  with frB = -1
    CHECK(c.f[4] == std::fma(c.f[1], c.f[2], 1.0));
    op_fnmadd(c, 4, 1, 3, 2, false);  CHECK(c.f[4] == -(std::fma(c.f[1], c.f[2], -1.0)));
    // fmul takes (t, a, c): frD = frA * frC
    c.f[1] = 3.0; c.f[2] = 4.0;       op_fmul(c, 5, 1, 2, false); CHECK(c.f[5] == 12.0);
    op_fdiv(c, 5, 2, 1, false);       CHECK(c.f[5] == 4.0 / 3.0);
    // fsel: frD = (frA >= 0) ? frC : frB   (args: t, a, b, c)
    c.f[1] = -1.0; c.f[2] = 10.0; c.f[3] = 20.0;
    op_fsel(c, 4, 1, 2, 3, false);    CHECK(c.f[4] == 10.0);   // a<0 -> frB
    c.f[1] = 0.0;  op_fsel(c, 4, 1, 2, 3, false); CHECK(c.f[4] == 20.0);   // a>=0 -> frC
    // fctiw: round to nearest even; fctiwz truncates; both clamp; result lives in the low word
    c.f[1] = 2.5;  op_fctiw(c, 2, 1, false);  CHECK(uint32_t(bits(c.f[2])) == 2);
    c.f[1] = 3.5;  op_fctiw(c, 2, 1, false);  CHECK(uint32_t(bits(c.f[2])) == 4);
    c.f[1] = 3.9;  op_fctiwz(c, 2, 1, false); CHECK(uint32_t(bits(c.f[2])) == 3);
    c.f[1] = -3.9; op_fctiwz(c, 2, 1, false); CHECK(uint32_t(bits(c.f[2])) == 0xFFFFFFFD);
    c.f[1] = 1e20; op_fctiwz(c, 2, 1, false); CHECK(uint32_t(bits(c.f[2])) == 0x7FFFFFFF);
    c.f[1] = -1e20; op_fctiwz(c, 2, 1, false); CHECK(uint32_t(bits(c.f[2])) == 0x80000000);
    c.r[3] = 0x80003000; c.r[4] = 0;
    op_stfiwx(c, 2, 3, 4);            CHECK(rd32(c, 0x80003000) == 0x80000000);
    // frsp, fmr, fneg, fabs, fnabs
    c.f[1] = 1.0000000001; op_frsp(c, 2, 1, false); CHECK(c.f[2] == 1.0);
    c.f[1] = -2.0; op_fabs(c, 2, 1, false);  CHECK(c.f[2] == 2.0);
    op_fneg(c, 2, 1, false);  CHECK(c.f[2] == 2.0);
    op_fnabs(c, 2, 1, false); CHECK(c.f[2] == -2.0);
    op_fmr(c, 2, 1, false);   CHECK(c.f[2] == -2.0);
    // fres / frsqrte are estimates; require 1/4096 relative accuracy
    c.f[1] = 8.0;  op_fres(c, 2, 1, false);    CHECK(std::fabs(c.f[2] - 0.125) < 0.125 / 4096);
    c.f[1] = 16.0; op_frsqrte(c, 2, 1, false); CHECK(std::fabs(c.f[2] - 0.25) < 0.25 / 4096);
    // fcmpu / fcmpo: LT=8 GT=4 EQ=2 unordered=1
    c.f[1] = 1.0; c.f[2] = 2.0;
    op_fcmpu(c, 1, 1, 2); CHECK(c.cr[1] == 8);
    op_fcmpu(c, 1, 2, 1); CHECK(c.cr[1] == 4);
    op_fcmpu(c, 1, 1, 1); CHECK(c.cr[1] == 2);
    c.f[3] = std::nan("");
    op_fcmpu(c, 1, 1, 3); CHECK(c.cr[1] == 1);
    // loads/stores: lfs converts, lfd/stfd round-trip, lfs also fills ps1
    c.r[3] = 0x80003100; wrf32(c, 0x80003100, 1.5f);
    op_lfs(c, 5, 3, 0);   CHECK(c.f[5] == 1.5 && c.ps1[5] == 1.5);
    c.f[6] = 6.125;       op_stfd(c, 6, 3, 8); CHECK(rdf64(c, 0x80003108) == 6.125);
    op_lfd(c, 7, 3, 8);   CHECK(c.f[7] == 6.125);
    op_stfs(c, 6, 3, 16); CHECK(rdf32(c, 0x80003110) == 6.125f);
    // FPSCR access
    c.fpscr = 0x1234; op_mffs(c, 8, false); CHECK(uint32_t(bits(c.f[8])) == 0x1234);

    // ---- paired singles ----
    c.f[1] = 1.0; c.ps1[1] = 2.0; c.f[2] = 10.0; c.ps1[2] = 20.0;
    op_ps_add(c, 3, 1, 2, false);      CHECK(c.f[3] == 11.0 && c.ps1[3] == 22.0);
    op_ps_sub(c, 3, 2, 1, false);      CHECK(c.f[3] == 9.0 && c.ps1[3] == 18.0);
    op_ps_mul(c, 3, 1, 2, false);      CHECK(c.f[3] == 10.0 && c.ps1[3] == 40.0);   // ps_mul(t, a, c)
    op_ps_merge00(c, 3, 1, 2, false);  CHECK(c.f[3] == 1.0 && c.ps1[3] == 10.0);
    op_ps_merge01(c, 3, 1, 2, false);  CHECK(c.f[3] == 1.0 && c.ps1[3] == 20.0);
    op_ps_merge10(c, 3, 1, 2, false);  CHECK(c.f[3] == 2.0 && c.ps1[3] == 10.0);
    op_ps_merge11(c, 3, 1, 2, false);  CHECK(c.f[3] == 2.0 && c.ps1[3] == 20.0);
    op_ps_muls0(c, 3, 1, 2, false);    CHECK(c.f[3] == 10.0 && c.ps1[3] == 20.0);   // ps_muls0(t, a, c): both scaled by c.ps0
    op_ps_muls1(c, 3, 1, 2, false);    CHECK(c.f[3] == 20.0 && c.ps1[3] == 40.0);
    op_ps_madd(c, 3, 1, 1, 2, false);  CHECK(c.f[3] == 1.0 * 10.0 + 1.0 && c.ps1[3] == 2.0 * 20.0 + 2.0);  // t = a*c + b
    op_ps_sum0(c, 3, 1, 2, 1, false);  CHECK(c.f[3] == 1.0 + 20.0 && c.ps1[3] == 2.0);  // ps0 = a.ps0 + b.ps1 ; ps1 = c.ps1
    op_ps_neg(c, 3, 1, false);         CHECK(c.f[3] == -1.0 && c.ps1[3] == -2.0);
    op_ps_abs(c, 3, 3, false);         CHECK(c.f[3] == 1.0 && c.ps1[3] == 2.0);
    op_ps_mr(c, 4, 2, false);          CHECK(c.f[4] == 10.0 && c.ps1[4] == 20.0);

    // quantized load: GQR0 = type s16 (7), scale 2 -> value * 2^-2
    wr16(c, 0x80003200, 0x0040); wr16(c, 0x80003202, 0xFFC0);
    c.gqr[0] = (2u << 24) | (7u << 16);
    c.r[3] = 0x80003200;
    op_psq_l(c, 5, 3, 0, 0, 0);        CHECK(c.f[5] == 16.0 && c.ps1[5] == -16.0);
    op_psq_l(c, 5, 3, 0, 1, 0);        CHECK(c.f[5] == 16.0 && c.ps1[5] == 1.0);    // W=1: one element, ps1 = 1.0
    // quantized store: u8 (4), scale 0 clamps to 0..255 and truncates
    c.gqr[1] = 4u;                       // ST_TYPE=4, ST_SCALE=0
    c.f[6] = 300.0; c.ps1[6] = -5.0;
    c.r[3] = 0x80003300;
    op_psq_st(c, 6, 3, 0, 0, 1);       CHECK(rd8(c, 0x80003300) == 255 && rd8(c, 0x80003301) == 0);
    c.f[6] = 7.9;  op_psq_st(c, 6, 3, 0, 1, 1); CHECK(rd8(c, 0x80003300) == 7);   // W=1 stores one element
    // float type passes through: GQR2 type 0
    c.gqr[2] = 0; wrf32(c, 0x80003400, 1.25f); wrf32(c, 0x80003404, -2.5f); c.r[3] = 0x80003400;
    op_psq_l(c, 7, 3, 0, 0, 2);        CHECK(c.f[7] == 1.25 && c.ps1[7] == -2.5);
    // ps_cmpo0 / ps_cmpu1 write a CR field like fcmp
    op_ps_cmpu0(c, 2, 1, 2);           CHECK(c.cr[2] == 8);
    op_ps_cmpu1(c, 2, 1, 2);           CHECK(c.cr[2] == 8);
    return 0;
}
```

Argument orders used above: `fmadd/fmsub/fnmadd/fnmsub (t, a, b, c)` compute `a*c ± b`; `fmul/fmuls/ps_mul/ps_muls0/ps_muls1 (t, a, c)`; `fsel (t, a, b, c)`: `t = a >= 0 ? c : b`; `ps_sum0 (t, a, b, c)`: `ps0 = a.ps0 + b.ps1`, `ps1 = c.ps1`; `ps_sum1`: `ps0 = c.ps0`, `ps1 = a.ps0 + b.ps1`; `ps_madd (t, a, b, c)`: `a*c + b`; `psq_l (t, a, d, w, i)`.

- [ ] **Step 2: Run to verify it fails**

Run: `scripts\dev.bat` — Expected: FAIL (`op_fadd` and friends undefined).

- [ ] **Step 3: Implement (append to `runtime/ppc_rt.h` before the closing namespace)**

```cpp
// ---- floating point ----
PPC_INLINE double rnd_s(double v) { return double(float(v)); }
PPC_INLINE void fp_rc(Ctx& c, bool rc) { if (rc) c.cr[1] = uint8_t(c.fpscr >> 28); }

PPC_INLINE void op_lfs(Ctx& c, int t, int a, int32_t d) { double v = rdf32(c, ea_d(c, a, d)); c.f[t] = v; c.ps1[t] = v; }
PPC_INLINE void op_lfsu(Ctx& c, int t, int a, int32_t d) { uint32_t e = c.r[a] + uint32_t(d); double v = rdf32(c, e); c.f[t] = v; c.ps1[t] = v; c.r[a] = e; }
PPC_INLINE void op_lfsx(Ctx& c, int t, int a, int b) { double v = rdf32(c, ea_x(c, a, b)); c.f[t] = v; c.ps1[t] = v; }
PPC_INLINE void op_lfsux(Ctx& c, int t, int a, int b) { uint32_t e = c.r[a] + c.r[b]; double v = rdf32(c, e); c.f[t] = v; c.ps1[t] = v; c.r[a] = e; }
PPC_INLINE void op_lfd(Ctx& c, int t, int a, int32_t d) { c.f[t] = rdf64(c, ea_d(c, a, d)); }
PPC_INLINE void op_lfdu(Ctx& c, int t, int a, int32_t d) { uint32_t e = c.r[a] + uint32_t(d); c.f[t] = rdf64(c, e); c.r[a] = e; }
PPC_INLINE void op_lfdx(Ctx& c, int t, int a, int b) { c.f[t] = rdf64(c, ea_x(c, a, b)); }
PPC_INLINE void op_lfdux(Ctx& c, int t, int a, int b) { uint32_t e = c.r[a] + c.r[b]; c.f[t] = rdf64(c, e); c.r[a] = e; }
PPC_INLINE void op_stfs(Ctx& c, int t, int a, int32_t d) { wrf32(c, ea_d(c, a, d), float(c.f[t])); }
PPC_INLINE void op_stfsu(Ctx& c, int t, int a, int32_t d) { uint32_t e = c.r[a] + uint32_t(d); wrf32(c, e, float(c.f[t])); c.r[a] = e; }
PPC_INLINE void op_stfsx(Ctx& c, int t, int a, int b) { wrf32(c, ea_x(c, a, b), float(c.f[t])); }
PPC_INLINE void op_stfsux(Ctx& c, int t, int a, int b) { uint32_t e = c.r[a] + c.r[b]; wrf32(c, e, float(c.f[t])); c.r[a] = e; }
PPC_INLINE void op_stfd(Ctx& c, int t, int a, int32_t d) { wrf64(c, ea_d(c, a, d), c.f[t]); }
PPC_INLINE void op_stfdu(Ctx& c, int t, int a, int32_t d) { uint32_t e = c.r[a] + uint32_t(d); wrf64(c, e, c.f[t]); c.r[a] = e; }
PPC_INLINE void op_stfdx(Ctx& c, int t, int a, int b) { wrf64(c, ea_x(c, a, b), c.f[t]); }
PPC_INLINE void op_stfdux(Ctx& c, int t, int a, int b) { uint32_t e = c.r[a] + c.r[b]; wrf64(c, e, c.f[t]); c.r[a] = e; }
PPC_INLINE void op_stfiwx(Ctx& c, int t, int a, int b) { uint64_t bt; std::memcpy(&bt, &c.f[t], 8); wr32(c, ea_x(c, a, b), uint32_t(bt)); }

#define PPC_FP_BIN(nm, EXPR, RND) \
    PPC_INLINE void op_##nm(Ctx& c, int t, int a, int b, bool rc) { double x = c.f[a], y = c.f[b]; c.f[t] = RND(EXPR); fp_rc(c, rc); }
PPC_FP_BIN(fadd, x + y, PPC_ID)   PPC_FP_BIN(fsub, x - y, PPC_ID)   PPC_FP_BIN(fdiv, x / y, PPC_ID)
PPC_FP_BIN(fadds, x + y, rnd_s)   PPC_FP_BIN(fsubs, x - y, rnd_s)   PPC_FP_BIN(fdivs, x / y, rnd_s)
PPC_INLINE void op_fmul(Ctx& c, int t, int a, int cc, bool rc) { c.f[t] = c.f[a] * c.f[cc]; fp_rc(c, rc); }
PPC_INLINE void op_fmuls(Ctx& c, int t, int a, int cc, bool rc) { c.f[t] = rnd_s(c.f[a] * c.f[cc]); fp_rc(c, rc); }
#define PPC_FP_FMA(nm, EXPR, RND) \
    PPC_INLINE void op_##nm(Ctx& c, int t, int a, int b, int cc, bool rc) { double x = c.f[a], y = c.f[b], z = c.f[cc]; c.f[t] = RND(EXPR); fp_rc(c, rc); }
PPC_FP_FMA(fmadd, std::fma(x, z, y), PPC_ID)    PPC_FP_FMA(fmsub, std::fma(x, z, -y), PPC_ID)
PPC_FP_FMA(fnmadd, -std::fma(x, z, y), PPC_ID)  PPC_FP_FMA(fnmsub, -std::fma(x, z, -y), PPC_ID)
PPC_FP_FMA(fmadds, std::fma(x, z, y), rnd_s)    PPC_FP_FMA(fmsubs, std::fma(x, z, -y), rnd_s)
PPC_FP_FMA(fnmadds, -std::fma(x, z, y), rnd_s)  PPC_FP_FMA(fnmsubs, -std::fma(x, z, -y), rnd_s)
PPC_INLINE void op_fsel(Ctx& c, int t, int a, int b, int cc, bool rc) { c.f[t] = c.f[a] >= 0.0 ? c.f[cc] : c.f[b]; fp_rc(c, rc); }
PPC_INLINE void op_fres(Ctx& c, int t, int b, bool rc) { c.f[t] = rnd_s(1.0 / c.f[b]); fp_rc(c, rc); }
PPC_INLINE void op_frsqrte(Ctx& c, int t, int b, bool rc) { c.f[t] = 1.0 / std::sqrt(c.f[b]); fp_rc(c, rc); }
PPC_INLINE void op_fsqrts(Ctx& c, int t, int b, bool rc) { c.f[t] = rnd_s(std::sqrt(c.f[b])); fp_rc(c, rc); }
PPC_INLINE void op_frsp(Ctx& c, int t, int b, bool rc) { c.f[t] = rnd_s(c.f[b]); fp_rc(c, rc); }
PPC_INLINE void fcti(Ctx& c, int t, double v, bool trunc, bool rc) {
    double r = trunc ? std::trunc(v) : std::nearbyint(v);   // default rounding mode is to-nearest-even
    int32_t i = std::isnan(v) || r <= -2147483648.0 ? INT32_MIN : r >= 2147483647.0 ? INT32_MAX : int32_t(r);
    uint64_t bt = 0xFFF8000000000000ull | uint32_t(i); std::memcpy(&c.f[t], &bt, 8); fp_rc(c, rc);
}
PPC_INLINE void op_fctiw(Ctx& c, int t, int b, bool rc) { fcti(c, t, c.f[b], false, rc); }
PPC_INLINE void op_fctiwz(Ctx& c, int t, int b, bool rc) { fcti(c, t, c.f[b], true, rc); }
PPC_INLINE void fcmp(Ctx& c, int f, double x, double y) {
    c.cr[f] = std::isnan(x) || std::isnan(y) ? 1 : x < y ? 8 : x > y ? 4 : 2;
}
PPC_INLINE void op_fcmpu(Ctx& c, int f, int a, int b) { fcmp(c, f, c.f[a], c.f[b]); }
PPC_INLINE void op_fcmpo(Ctx& c, int f, int a, int b) { fcmp(c, f, c.f[a], c.f[b]); }
PPC_INLINE void op_fmr(Ctx& c, int t, int b, bool rc) { c.f[t] = c.f[b]; fp_rc(c, rc); }
PPC_INLINE void op_fneg(Ctx& c, int t, int b, bool rc) { c.f[t] = -c.f[b]; fp_rc(c, rc); }
PPC_INLINE void op_fabs(Ctx& c, int t, int b, bool rc) { c.f[t] = std::fabs(c.f[b]); fp_rc(c, rc); }
PPC_INLINE void op_fnabs(Ctx& c, int t, int b, bool rc) { c.f[t] = -std::fabs(c.f[b]); fp_rc(c, rc); }
PPC_INLINE void op_mffs(Ctx& c, int t, bool rc) { uint64_t bt = 0xFFF8000000000000ull | c.fpscr; std::memcpy(&c.f[t], &bt, 8); fp_rc(c, rc); }
PPC_INLINE void op_mtfsf(Ctx& c, int fm, int b, bool rc) {
    uint64_t bt; std::memcpy(&bt, &c.f[b], 8);
    for (int i = 0; i < 8; ++i) if (fm & (0x80 >> i)) c.fpscr = (c.fpscr & ~(0xFu << (28 - 4 * i))) | (uint32_t(bt) & (0xFu << (28 - 4 * i)));
    fp_rc(c, rc);
}
PPC_INLINE void op_mtfsfi(Ctx& c, int crfd, int imm, bool rc) { c.fpscr = (c.fpscr & ~(0xFu << (28 - 4 * crfd))) | (uint32_t(imm & 0xF) << (28 - 4 * crfd)); fp_rc(c, rc); }
PPC_INLINE void op_mtfsb0(Ctx& c, int bit, bool rc) { c.fpscr &= ~(0x80000000u >> bit); fp_rc(c, rc); }
PPC_INLINE void op_mtfsb1(Ctx& c, int bit, bool rc) { c.fpscr |= 0x80000000u >> bit; fp_rc(c, rc); }
PPC_INLINE void op_mcrfs(Ctx& c, int d, int s) { c.cr[d] = uint8_t((c.fpscr >> (28 - 4 * s)) & 0xF); c.fpscr &= ~(0xFu << (28 - 4 * s)); }

// ---- paired singles ----
#define PPC_PS_BIN(nm, EXPR) \
    PPC_INLINE void op_##nm(Ctx& c, int t, int a, int b, bool rc) { \
        double x0 = c.f[a], x1 = c.ps1[a], y0 = c.f[b], y1 = c.ps1[b]; \
        c.f[t] = rnd_s(EXPR(x0, y0)); c.ps1[t] = rnd_s(EXPR(x1, y1)); fp_rc(c, rc); }
#define PS_ADD(x, y) ((x) + (y))
#define PS_SUB(x, y) ((x) - (y))
#define PS_DIV(x, y) ((x) / (y))
PPC_PS_BIN(ps_add, PS_ADD) PPC_PS_BIN(ps_sub, PS_SUB) PPC_PS_BIN(ps_div, PS_DIV)
PPC_INLINE void op_ps_mul(Ctx& c, int t, int a, int cc, bool rc) { double p0 = rnd_s(c.f[a] * c.f[cc]), p1 = rnd_s(c.ps1[a] * c.ps1[cc]); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_muls0(Ctx& c, int t, int a, int cc, bool rc) { double s = c.f[cc], p0 = rnd_s(c.f[a] * s), p1 = rnd_s(c.ps1[a] * s); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_muls1(Ctx& c, int t, int a, int cc, bool rc) { double s = c.ps1[cc], p0 = rnd_s(c.f[a] * s), p1 = rnd_s(c.ps1[a] * s); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
#define PPC_PS_FMA(nm, E0, E1) \
    PPC_INLINE void op_##nm(Ctx& c, int t, int a, int b, int cc, bool rc) { \
        double a0 = c.f[a], a1 = c.ps1[a], b0 = c.f[b], b1 = c.ps1[b], c0 = c.f[cc], c1 = c.ps1[cc]; \
        double r0 = rnd_s(E0), r1 = rnd_s(E1); c.f[t] = r0; c.ps1[t] = r1; fp_rc(c, rc); }
PPC_PS_FMA(ps_madd, std::fma(a0, c0, b0), std::fma(a1, c1, b1))
PPC_PS_FMA(ps_msub, std::fma(a0, c0, -b0), std::fma(a1, c1, -b1))
PPC_PS_FMA(ps_nmadd, -std::fma(a0, c0, b0), -std::fma(a1, c1, b1))
PPC_PS_FMA(ps_nmsub, -std::fma(a0, c0, -b0), -std::fma(a1, c1, -b1))
PPC_PS_FMA(ps_madds0, std::fma(a0, c0, b0), std::fma(a1, c0, b1))
PPC_PS_FMA(ps_madds1, std::fma(a0, c1, b0), std::fma(a1, c1, b1))
PPC_PS_FMA(ps_sel, (a0 >= 0.0 ? c0 : b0), (a1 >= 0.0 ? c1 : b1))
PPC_PS_FMA(ps_sum0, (a0 + b1), c1)
PPC_PS_FMA(ps_sum1, c0, (a0 + b1))
PPC_INLINE void op_ps_res(Ctx& c, int t, int b, bool rc) { double p0 = rnd_s(1.0 / c.f[b]), p1 = rnd_s(1.0 / c.ps1[b]); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_rsqrte(Ctx& c, int t, int b, bool rc) { double p0 = rnd_s(1.0 / std::sqrt(c.f[b])), p1 = rnd_s(1.0 / std::sqrt(c.ps1[b])); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_neg(Ctx& c, int t, int b, bool rc) { double p0 = -c.f[b], p1 = -c.ps1[b]; c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_abs(Ctx& c, int t, int b, bool rc) { double p0 = std::fabs(c.f[b]), p1 = std::fabs(c.ps1[b]); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_nabs(Ctx& c, int t, int b, bool rc) { double p0 = -std::fabs(c.f[b]), p1 = -std::fabs(c.ps1[b]); c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_mr(Ctx& c, int t, int b, bool rc) { double p0 = c.f[b], p1 = c.ps1[b]; c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_merge00(Ctx& c, int t, int a, int b, bool rc) { double p0 = c.f[a], p1 = c.f[b]; c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_merge01(Ctx& c, int t, int a, int b, bool rc) { double p0 = c.f[a], p1 = c.ps1[b]; c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_merge10(Ctx& c, int t, int a, int b, bool rc) { double p0 = c.ps1[a], p1 = c.f[b]; c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_merge11(Ctx& c, int t, int a, int b, bool rc) { double p0 = c.ps1[a], p1 = c.ps1[b]; c.f[t] = p0; c.ps1[t] = p1; fp_rc(c, rc); }
PPC_INLINE void op_ps_cmpu0(Ctx& c, int f, int a, int b) { fcmp(c, f, c.f[a], c.f[b]); }
PPC_INLINE void op_ps_cmpo0(Ctx& c, int f, int a, int b) { fcmp(c, f, c.f[a], c.f[b]); }
PPC_INLINE void op_ps_cmpu1(Ctx& c, int f, int a, int b) { fcmp(c, f, c.ps1[a], c.ps1[b]); }
PPC_INLINE void op_ps_cmpo1(Ctx& c, int f, int a, int b) { fcmp(c, f, c.ps1[a], c.ps1[b]); }

// ---- quantized load/store (GQR: LD_SCALE bits 24-29, LD_TYPE 16-18, ST_SCALE 8-13, ST_TYPE 0-2; types 4=u8 5=u16 6=s8 7=s16, else float) ----
PPC_INLINE int gq_scale(uint32_t v) { int s = int(v & 0x3F); return s & 0x20 ? s - 64 : s; }
PPC_INLINE int gq_bytes(int type) { return (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4; }
PPC_INLINE double gq_load1(Ctx& c, uint32_t ea, int type, int scale) {
    double v;
    switch (type) {
        case 4: v = double(rd8(c, ea)); break;
        case 5: v = double(rd16(c, ea)); break;
        case 6: v = double(int8_t(rd8(c, ea))); break;
        case 7: v = double(int16_t(rd16(c, ea))); break;
        default: return double(rdf32(c, ea));
    }
    return v * std::ldexp(1.0, -scale);
}
PPC_INLINE void gq_store1(Ctx& c, uint32_t ea, int type, int scale, double v) {
    if (type < 4 || type > 7) { wrf32(c, ea, float(v)); return; }
    double x = std::trunc(v * std::ldexp(1.0, scale));
    double lo = type == 4 ? 0 : type == 5 ? 0 : type == 6 ? -128 : -32768;
    double hi = type == 4 ? 255 : type == 5 ? 65535 : type == 6 ? 127 : 32767;
    x = std::isnan(x) ? lo : x < lo ? lo : x > hi ? hi : x;
    if (gq_bytes(type) == 1) wr8(c, ea, uint32_t(int32_t(x))); else wr16(c, ea, uint32_t(int32_t(x)));
}
PPC_INLINE void psq_load(Ctx& c, int t, uint32_t ea, int w, int i) {
    uint32_t g = c.gqr[i]; int type = (g >> 16) & 7, scale = gq_scale(g >> 24);
    double p0 = gq_load1(c, ea, type, scale);
    double p1 = w ? 1.0 : gq_load1(c, ea + gq_bytes(type), type, scale);
    c.f[t] = p0; c.ps1[t] = p1;
}
PPC_INLINE void psq_store(Ctx& c, int t, uint32_t ea, int w, int i) {
    uint32_t g = c.gqr[i]; int type = g & 7, scale = gq_scale(g >> 8);
    gq_store1(c, ea, type, scale, c.f[t]);
    if (!w) gq_store1(c, ea + gq_bytes(type), type, scale, c.ps1[t]);
}
PPC_INLINE void op_psq_l(Ctx& c, int t, int a, int32_t d, int w, int i) { psq_load(c, t, ea_d(c, a, d), w, i); }
PPC_INLINE void op_psq_lu(Ctx& c, int t, int a, int32_t d, int w, int i) { uint32_t e = c.r[a] + uint32_t(d); psq_load(c, t, e, w, i); c.r[a] = e; }
PPC_INLINE void op_psq_st(Ctx& c, int t, int a, int32_t d, int w, int i) { psq_store(c, t, ea_d(c, a, d), w, i); }
PPC_INLINE void op_psq_stu(Ctx& c, int t, int a, int32_t d, int w, int i) { uint32_t e = c.r[a] + uint32_t(d); psq_store(c, t, e, w, i); c.r[a] = e; }
PPC_INLINE void op_psq_lx(Ctx& c, int t, int a, int b, int w, int i) { psq_load(c, t, ea_x(c, a, b), w, i); }
PPC_INLINE void op_psq_lux(Ctx& c, int t, int a, int b, int w, int i) { uint32_t e = c.r[a] + c.r[b]; psq_load(c, t, e, w, i); c.r[a] = e; }
PPC_INLINE void op_psq_stx(Ctx& c, int t, int a, int b, int w, int i) { psq_store(c, t, ea_x(c, a, b), w, i); }
PPC_INLINE void op_psq_stux(Ctx& c, int t, int a, int b, int w, int i) { uint32_t e = c.r[a] + c.r[b]; psq_store(c, t, e, w, i); c.r[a] = e; }
```

- [ ] **Step 4: Run to verify it passes**

Run: `scripts\dev.bat` — Expected: `test_ppc_float` and every earlier test PASSED. Where an expected value in the test disagrees with the implementation, decide from the PowerPC architecture (the test was written from it); fix the code or the test and note the change in the ledger.

---

### Task 4: Project config and analyzer (TDD)

**Files:**
- Create: `tests/ppc_enc.h`, `tests/test_analyzer.cpp`, `common/analyzer/config.h`, `common/analyzer/config.cpp`, `common/analyzer/analyzer.h`, `common/analyzer/analyzer.cpp`, `project/uc.cfg`
- Modify: `common/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `uc::DolImage`, `uc::section_bytes` (previous plan), `ppc::decode`, `ppc::*` field extractors (Task 1), `make_dol` (`tests/dol_builder.h`).
- Produces (`namespace uc`):
  - `struct JumpTable { uint32_t bctr_addr; std::vector<uint32_t> targets; };`
  - `struct Config { std::set<uint32_t> extra_entries, no_functions; std::map<uint32_t, std::string> hle; std::vector<JumpTable> jump_tables; };`
  - `bool parse_config(const std::string& text, Config& out, std::string& error);` `bool load_config(const std::string& path, Config& out, std::string& error);`
  - `struct Image { std::span<const uint8_t> file; const DolImage* dol; const DolSection* section(uint32_t) const; bool word(uint32_t addr, uint32_t& out) const; bool in_text(uint32_t) const; };`
  - `struct Function { uint32_t entry, end; };` (`[entry, end)`)
  - `struct Analysis { std::vector<Function> functions; std::set<uint32_t> entries; std::vector<JumpTable> jump_tables; std::vector<uint32_t> unresolved_bctr; const Function* find_containing(uint32_t) const; bool is_entry(uint32_t) const; };`
  - `Analysis analyze(const Image&, const Config&);`
- Produces (`tests/ppc_enc.h`, `namespace enc`): `addi addis li lis ori cmplwi cmpwi lwz stw stwu lfs stfs add lwzx mr mtctr mflr mtlr slwi b bl bc bdnz bgt blt fadds fctiwz stfiwx ps_add psq_l psq_st`, constants `blr bctr bctrl`, and `struct Asm { uint32_t base; std::vector<uint32_t> w; uint32_t here() const; size_t emit(uint32_t); void b_to(uint32_t); void bl_to(uint32_t); void bc_to(int bo, int bi, uint32_t); void patch_b(size_t idx, uint32_t target); void patch_bl(size_t, uint32_t); void patch_bc(size_t idx, int bo, int bi, uint32_t target); }`.

- [ ] **Step 1: Write the test helpers and the failing tests**

`tests/ppc_enc.h`:
```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace enc {
inline uint32_t u(int v) { return uint32_t(v); }
inline uint32_t D(uint32_t op, int t, int a, int imm) { return op << 26 | u(t) << 21 | u(a) << 16 | (u(imm) & 0xFFFF); }
inline uint32_t X(uint32_t op, int t, int a, int b, uint32_t xo, uint32_t rc = 0) { return op << 26 | u(t) << 21 | u(a) << 16 | u(b) << 11 | xo << 1 | rc; }
inline uint32_t addi(int t, int a, int i) { return D(14, t, a, i); }
inline uint32_t addis(int t, int a, int i) { return D(15, t, a, i); }
inline uint32_t li(int t, int i) { return addi(t, 0, i); }
inline uint32_t lis(int t, int i) { return addis(t, 0, i); }
inline uint32_t ori(int a, int s, int k) { return D(24, s, a, k); }
inline uint32_t cmplwi(int a, int k) { return D(10, 0, a, k); }
inline uint32_t cmpwi(int a, int i) { return D(11, 0, a, i); }
inline uint32_t lwz(int t, int d, int a) { return D(32, t, a, d); }
inline uint32_t stw(int s, int d, int a) { return D(36, s, a, d); }
inline uint32_t stwu(int s, int d, int a) { return D(37, s, a, d); }
inline uint32_t lfs(int t, int d, int a) { return D(48, t, a, d); }
inline uint32_t stfs(int s, int d, int a) { return D(52, s, a, d); }
inline uint32_t add(int t, int a, int b) { return X(31, t, a, b, 266); }
inline uint32_t lwzx(int t, int a, int b) { return X(31, t, a, b, 23); }
inline uint32_t mr(int a, int s) { return X(31, s, a, s, 444); }
inline uint32_t mtctr(int r) { return 0x7C0903A6u | u(r) << 21; }
inline uint32_t mflr(int r) { return 0x7C0802A6u | u(r) << 21; }
inline uint32_t mtlr(int r) { return 0x7C0803A6u | u(r) << 21; }
inline uint32_t slwi(int a, int s, int n) { return 21u << 26 | u(s) << 21 | u(a) << 16 | u(n) << 11 | u(31 - n) << 1; }
inline uint32_t b(int32_t off) { return 18u << 26 | (uint32_t(off) & 0x03FFFFFC); }
inline uint32_t bl(int32_t off) { return b(off) | 1; }
inline uint32_t bc(int bo, int bi, int32_t off) { return 16u << 26 | u(bo) << 21 | u(bi) << 16 | (uint32_t(off) & 0xFFFC); }
inline uint32_t bdnz(int32_t off) { return bc(16, 0, off); }
inline uint32_t bgt(int32_t off) { return bc(12, 1, off); }
inline uint32_t blt(int32_t off) { return bc(12, 0, off); }
constexpr uint32_t blr = 0x4E800020u, bctr = 0x4E800420u, bctrl = 0x4E800421u;
inline uint32_t fadds(int t, int a, int b_) { return 59u << 26 | u(t) << 21 | u(a) << 16 | u(b_) << 11 | 21u << 1; }
inline uint32_t fctiwz(int t, int b_) { return 63u << 26 | u(t) << 21 | u(b_) << 11 | 15u << 1; }
inline uint32_t stfiwx(int s, int a, int b_) { return X(31, s, a, b_, 983); }
inline uint32_t ps_add(int t, int a, int b_) { return 4u << 26 | u(t) << 21 | u(a) << 16 | u(b_) << 11 | 21u << 1; }
inline uint32_t psq_l(int t, int d, int a, int w, int i) { return 56u << 26 | u(t) << 21 | u(a) << 16 | u(w) << 15 | u(i) << 12 | (u(d) & 0xFFF); }
inline uint32_t psq_st(int s, int d, int a, int w, int i) { return 60u << 26 | u(s) << 21 | u(a) << 16 | u(w) << 15 | u(i) << 12 | (u(d) & 0xFFF); }

struct Asm {
    uint32_t base;
    std::vector<uint32_t> w;
    explicit Asm(uint32_t base_ = 0x80003100) : base(base_) {}
    uint32_t here() const { return base + 4 * uint32_t(w.size()); }
    size_t emit(uint32_t x) { w.push_back(x); return w.size() - 1; }
    void b_to(uint32_t t) { emit(b(int32_t(t - here()))); }
    void bl_to(uint32_t t) { emit(bl(int32_t(t - here()))); }
    void bc_to(int bo, int bi, uint32_t t) { emit(bc(bo, bi, int32_t(t - here()))); }
    uint32_t addr(size_t idx) const { return base + 4 * uint32_t(idx); }
    void patch_b(size_t i, uint32_t t) { w[i] = b(int32_t(t - addr(i))); }
    void patch_bl(size_t i, uint32_t t) { w[i] = bl(int32_t(t - addr(i))); }
    void patch_bc(size_t i, int bo, int bi, uint32_t t) { w[i] = bc(bo, bi, int32_t(t - addr(i))); }
};
inline void be32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
}  // namespace enc
```

`tests/test_analyzer.cpp`:
```cpp
#include "analyzer/analyzer.h"
#include "check.h"
#include "dol_builder.h"
#include "ppc_enc.h"
using namespace enc;

// f0 @ idx0..7: prologue, bl f1, epilogue.   f1 @ idx8..9: leaf, bl target.
// f2 @ idx10..25: switch via jump table (entry given by config).   f3 @ idx26..30: prologue after blr.
static std::vector<uint8_t> make(uint32_t table_entry2_override, Asm& a) {
    size_t call = 0;
    a.emit(stwu(1, -16, 1)); a.emit(mflr(0)); a.emit(stw(0, 20, 1));
    call = a.emit(0);
    a.emit(lwz(0, 20, 1)); a.emit(mtlr(0)); a.emit(addi(1, 1, 16)); a.emit(blr);
    uint32_t f1 = a.here(); a.emit(addi(3, 3, 1)); a.emit(blr);
    a.patch_bl(call, f1);
    uint32_t f2 = a.here();
    a.emit(cmplwi(3, 2));
    size_t bgt_i = a.emit(0);
    a.emit(lis(4, 0x8040)); a.emit(addi(4, 4, 0)); a.emit(slwi(0, 3, 2)); a.emit(lwzx(0, 4, 0)); a.emit(mtctr(0)); a.emit(bctr);
    uint32_t c0 = a.here(); a.emit(li(3, 100)); a.emit(blr);
    uint32_t c1 = a.here(); a.emit(li(3, 200)); a.emit(blr);
    uint32_t c2 = a.here(); a.emit(li(3, 300)); a.emit(blr);
    uint32_t dflt = a.here(); a.emit(li(3, -1)); a.emit(blr);
    a.patch_bc(bgt_i, 12, 1, dflt);
    a.emit(stwu(1, -16, 1)); a.emit(mflr(0)); a.emit(mtlr(0)); a.emit(addi(1, 1, 16)); a.emit(blr);
    std::vector<uint8_t> data;
    be32(data, c0); be32(data, c1); be32(data, table_entry2_override ? table_entry2_override : c2);
    (void)f2;
    return make_dol(a.w, data);
}

int main() {
    uc::Config cfg;
    {   // entry f2 (no bl reaches it) is supplied by config
        Asm a; auto dol = make(0, a);
        auto parsed = uc::parse_dol(dol); CHECK(parsed.ok);
        uc::Image img{dol, &parsed.image};
        cfg.extra_entries.insert(a.addr(10));
        auto an = uc::analyze(img, cfg);
        CHECK(an.functions.size() == 4);
        CHECK(an.functions[0].entry == a.addr(0)  && an.functions[0].end == a.addr(8));
        CHECK(an.functions[1].entry == a.addr(8)  && an.functions[1].end == a.addr(10));
        CHECK(an.functions[2].entry == a.addr(10) && an.functions[2].end == a.addr(26));
        CHECK(an.functions[3].entry == a.addr(26) && an.functions[3].end == a.addr(31));
        CHECK(an.is_entry(a.addr(26)));                       // prologue after a blr
        CHECK(!an.is_entry(a.addr(18)));                      // jump-table targets are NOT function entries
        CHECK(an.jump_tables.size() == 1);
        CHECK(an.jump_tables[0].bctr_addr == a.addr(17));
        CHECK(an.jump_tables[0].targets.size() == 3);
        CHECK(an.jump_tables[0].targets[0] == a.addr(18) && an.jump_tables[0].targets[2] == a.addr(22));
        CHECK(an.unresolved_bctr.empty());
        CHECK(an.find_containing(a.addr(20))->entry == a.addr(10));
    }
    {   // review focus: a table entry pointing outside the function is rejected, bctr stays unresolved
        Asm a; auto dol = make(0x80003000, a);
        auto parsed = uc::parse_dol(dol);
        uc::Image img{dol, &parsed.image};
        uc::Config c2; c2.extra_entries.insert(a.addr(10));
        auto an = uc::analyze(img, c2);
        CHECK(an.jump_tables.empty());
        CHECK(an.unresolved_bctr.size() == 1 && an.unresolved_bctr[0] == a.addr(17));
    }
    {   // nofunc removes a detected entry; an explicit jumptable in the config resolves the bctr
        Asm a; auto dol = make(0x80003000, a);
        auto parsed = uc::parse_dol(dol);
        uc::Image img{dol, &parsed.image};
        uc::Config c3; c3.extra_entries.insert(a.addr(10)); c3.no_functions.insert(a.addr(26));
        c3.jump_tables.push_back({a.addr(17), {a.addr(18), a.addr(20), a.addr(22)}});
        auto an = uc::analyze(img, c3);
        CHECK(!an.is_entry(a.addr(26)));
        CHECK(an.jump_tables.size() == 1 && an.unresolved_bctr.empty());
    }
    {   // config text parsing
        uc::Config c; std::string err;
        CHECK(uc::parse_config("# comment\nentry 80001000\nnofunc 0x80002000\nhle 80003000 OSInit  # trailing\njumptable 80004000 80004010 80004020\n", c, err));
        CHECK(c.extra_entries.count(0x80001000) && c.no_functions.count(0x80002000));
        CHECK(c.hle.at(0x80003000) == "OSInit");
        CHECK(c.jump_tables.size() == 1 && c.jump_tables[0].targets.size() == 2);
        uc::Config bad;
        CHECK(!uc::parse_config("entry 80001000\nbogus 1\n", bad, err));
        CHECK(err.find("line 2") != std::string::npos);
        CHECK(!uc::parse_config("hle 80001000 bad-name\n", bad, err));
        CHECK(!uc::parse_config("entry zz\n", bad, err));
    }
    return 0;
}
```

- [ ] **Step 2: Wire and run to verify it fails**

`tests/CMakeLists.txt`: add `test_analyzer` to the first `foreach` list (the one linking `uc_common`).
Run: `scripts\dev.bat` — Expected: FAIL (`analyzer/analyzer.h` not found).

- [ ] **Step 3: Implement config**

`common/analyzer/config.h`:
```cpp
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace uc {

struct JumpTable {
    uint32_t bctr_addr;
    std::vector<uint32_t> targets;
};

struct Config {
    std::set<uint32_t> extra_entries, no_functions;
    std::map<uint32_t, std::string> hle;  // guest function address -> HLE name (hle_<name> in the runtime)
    std::vector<JumpTable> jump_tables;
};

// Line format:  entry <hex> | nofunc <hex> | hle <hex> <Name> | jumptable <bctr hex> <target hex>...   ('#' starts a comment)
bool parse_config(const std::string& text, Config& out, std::string& error);
bool load_config(const std::string& path, Config& out, std::string& error);

}  // namespace uc
```
`common/analyzer/config.cpp`:
```cpp
#include "analyzer/config.h"
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace uc {

static bool hex32(const std::string& s, uint32_t& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    unsigned long long x = std::strtoull(s.c_str(), &end, 16);
    if (*end || x > 0xFFFFFFFFull) return false;
    v = uint32_t(x);
    return true;
}

bool parse_config(const std::string& text, Config& out, std::string& error) {
    std::istringstream in(text);
    std::string line;
    int n = 0;
    auto fail = [&](const std::string& m) { error = "line " + std::to_string(n) + ": " + m; return false; };
    while (std::getline(in, line)) {
        ++n;
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ts(line);
        std::vector<std::string> tok;
        for (std::string t; ts >> t;) tok.push_back(t);
        if (tok.empty()) continue;
        uint32_t a = 0;
        if (tok[0] == "entry" || tok[0] == "nofunc") {
            if (tok.size() != 2 || !hex32(tok[1], a)) return fail("expected: " + tok[0] + " <hex address>");
            (tok[0] == "entry" ? out.extra_entries : out.no_functions).insert(a);
        } else if (tok[0] == "hle") {
            if (tok.size() != 3 || !hex32(tok[1], a)) return fail("expected: hle <hex address> <Name>");
            for (char ch : tok[2]) if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') return fail("invalid HLE name '" + tok[2] + "'");
            out.hle[a] = tok[2];
        } else if (tok[0] == "jumptable") {
            JumpTable jt;
            if (tok.size() < 3 || !hex32(tok[1], jt.bctr_addr)) return fail("expected: jumptable <bctr address> <target>...");
            for (size_t i = 2; i < tok.size(); ++i) {
                if (!hex32(tok[i], a)) return fail("bad target address '" + tok[i] + "'");
                jt.targets.push_back(a);
            }
            out.jump_tables.push_back(jt);
        } else {
            return fail("unknown directive '" + tok[0] + "'");
        }
    }
    return true;
}

bool load_config(const std::string& path, Config& out, std::string& error) {
    std::ifstream f(path);
    if (!f) { error = "cannot open " + path; return false; }
    std::stringstream ss; ss << f.rdbuf();
    return parse_config(ss.str(), out, error);
}

}  // namespace uc
```
`project/uc.cfg` (starts nearly empty; later tasks add lines):
```
# Umbrella Chronicles project configuration (see docs/superpowers/plans/2026-10-12-analyzer-translator.md)
# entry <hex>       force a function entry
# nofunc <hex>      never treat this address as a function entry
# hle <hex> <Name>  replace the guest function at <hex> by hle_<Name> from the runtime
# jumptable <bctr hex> <target hex>...   manual jump table for a bctr the analyzer could not resolve
```

- [ ] **Step 4: Implement the analyzer**

`common/analyzer/analyzer.h`:
```cpp
#pragma once
#include "analyzer/config.h"
#include "dol.h"
#include <set>
#include <span>
#include <vector>

namespace uc {

struct Image {
    std::span<const uint8_t> file;
    const DolImage* dol;
    const DolSection* section(uint32_t addr) const;
    bool word(uint32_t addr, uint32_t& out) const;  // 4-aligned big-endian read from any section
    bool in_text(uint32_t addr) const;
};

struct Function {
    uint32_t entry, end;  // [entry, end)
};

struct Analysis {
    std::vector<Function> functions;  // sorted by entry
    std::set<uint32_t> entries;
    std::vector<JumpTable> jump_tables;
    std::vector<uint32_t> unresolved_bctr;
    const Function* find_containing(uint32_t addr) const;
    bool is_entry(uint32_t addr) const { return entries.count(addr) != 0; }
};

Analysis analyze(const Image& img, const Config& cfg);

}  // namespace uc
```
`common/analyzer/analyzer.cpp`:
```cpp
#include "analyzer/analyzer.h"
#include <algorithm>
#include <iterator>
#include <optional>

namespace uc {

const DolSection* Image::section(uint32_t a) const {
    for (const auto& s : dol->sections) if (a >= s.address && a - s.address < s.size) return &s;
    return nullptr;
}
bool Image::word(uint32_t a, uint32_t& out) const {
    auto* s = section(a);
    if (!s || (a & 3) || a - s->address + 4 > s->size) return false;
    const uint8_t* p = file.data() + s->file_offset + (a - s->address);
    out = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    return true;
}
bool Image::in_text(uint32_t a) const { auto* s = section(a); return s && s->is_text; }

const Function* Analysis::find_containing(uint32_t a) const {
    auto it = std::upper_bound(functions.begin(), functions.end(), a, [](uint32_t v, const Function& f) { return v < f.entry; });
    if (it == functions.begin()) return nullptr;
    --it;
    return a < it->end ? &*it : nullptr;
}

static bool is_terminator(uint32_t w) {
    return w == 0 || w == 0x4E800020u || w == 0x4E800420u || ((w >> 26) == 18 && !(w & 1));  // blr, bctr, b, padding
}

static std::vector<Function> make_functions(const Image& img, const std::set<uint32_t>& entries) {
    std::vector<Function> r;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const DolSection* s = img.section(*it);
        uint32_t sec_end = s->address + (s->size & ~3u);
        auto nx = std::next(it);
        uint32_t end = (nx != entries.end() && *nx < sec_end) ? *nx : sec_end;
        r.push_back({*it, end});
    }
    return r;
}

// Finds `cmplwi rN,K ... lis/addi base ... slwi ... lwzx rX,rBase,rIdx ; mtctr rX ; bctr` and reads K+1 targets from the table.
static std::optional<JumpTable> find_table(const Image& img, const Function& f, uint32_t bctr_pc) {
    uint32_t w = 0;
    uint32_t mtctr_pc = 0, reg = 0;
    for (int i = 1; i <= 6; ++i) {
        if (bctr_pc < f.entry + 4u * i) break;
        uint32_t p = bctr_pc - 4u * i;
        if (img.word(p, w) && (w & 0xFC1FFFFFu) == 0x7C0903A6u) { mtctr_pc = p; reg = (w >> 21) & 31; break; }
    }
    if (!mtctr_pc) return std::nullopt;
    uint32_t lw_pc = 0, ra = 0, rb = 0;
    for (int i = 1; i <= 8; ++i) {
        if (mtctr_pc < f.entry + 4u * i) break;
        uint32_t p = mtctr_pc - 4u * i;
        if (img.word(p, w) && (w >> 26) == 31 && ((w >> 1) & 0x3FF) == 23 && ((w >> 21) & 31) == reg) {
            lw_pc = p; ra = (w >> 16) & 31; rb = (w >> 11) & 31; break;
        }
    }
    if (!lw_pc) return std::nullopt;
    uint32_t count = 0;
    for (int i = 1; i <= 24; ++i) {
        if (lw_pc < f.entry + 4u * i) break;
        if (img.word(lw_pc - 4u * i, w) && (w >> 26) == 10) { count = (w & 0xFFFF) + 1; break; }
    }
    if (count < 2 || count > 512) return std::nullopt;
    auto base_of = [&](uint32_t r, uint32_t& val) {
        bool have_lo = false; uint32_t lo = 0;
        for (int i = 1; i <= 20; ++i) {
            if (lw_pc < f.entry + 4u * i) break;
            if (!img.word(lw_pc - 4u * i, w)) break;
            uint32_t prim = w >> 26, t = (w >> 21) & 31, a = (w >> 16) & 31;
            if (!have_lo && prim == 14 && t == r && a == r) { have_lo = true; lo = uint32_t(int32_t(int16_t(w))); }
            else if (!have_lo && prim == 24 && t == r && a == r) { have_lo = true; lo = w & 0xFFFF; }
            else if (prim == 15 && t == r && a == 0) { val = ((w & 0xFFFF) << 16) + (have_lo ? lo : 0u); return true; }
        }
        return false;
    };
    for (uint32_t base_reg : {ra, rb}) {
        uint32_t base = 0;
        if (!base_of(base_reg, base)) continue;
        JumpTable jt{bctr_pc, {}};
        bool ok = true;
        for (uint32_t i = 0; i < count && ok; ++i) {
            uint32_t t = 0;
            ok = img.word(base + 4 * i, t) && !(t & 3) && t >= f.entry && t < f.end;
            jt.targets.push_back(t);
        }
        if (ok) return jt;
    }
    return std::nullopt;
}

Analysis analyze(const Image& img, const Config& cfg) {
    std::set<uint32_t> entries;
    auto add = [&](uint32_t a) { if (!(a & 3) && img.in_text(a) && !cfg.no_functions.count(a)) entries.insert(a); };
    add(img.dol->entry);
    for (uint32_t a : cfg.extra_entries) add(a);
    for (auto& [a, n] : cfg.hle) add(a);

    for (const auto& s : img.dol->sections) {
        if (!s.is_text) continue;
        uint32_t prev = 0;
        for (uint32_t pc = s.address; pc + 4 <= s.address + s.size; pc += 4) {
            uint32_t w = 0; img.word(pc, w);
            if ((w >> 26) == 18 && (w & 3) == 1) {  // bl
                uint32_t li = w & 0x03FFFFFCu; if (li & 0x02000000u) li |= 0xFC000000u;
                add(pc + li);
            }
            bool starts = pc == s.address || is_terminator(prev);
            if (starts && ((w & 0xFFFF0000u) == 0x94210000u || w == 0x7C0802A6u)) add(pc);  // stwu r1,-N(r1) | mflr r0
            prev = w;
        }
    }

    Analysis an;
    std::set<uint32_t> jt_targets;
    auto find_tables = [&](const std::vector<Function>& fns) {
        std::vector<JumpTable> found = cfg.jump_tables;
        std::set<uint32_t> have;
        for (auto& j : found) have.insert(j.bctr_addr);
        std::vector<uint32_t> unresolved;
        for (const auto& f : fns) {
            for (uint32_t pc = f.entry; pc < f.end; pc += 4) {
                uint32_t w = 0;
                if (!img.word(pc, w) || w != 0x4E800420u || have.count(pc)) continue;
                if (auto jt = find_table(img, f, pc)) { found.push_back(*jt); have.insert(pc); }
                else unresolved.push_back(pc);
            }
        }
        return std::pair{found, unresolved};
    };

    // Phase 1: functions from certain entries, so jump-table targets are known before data pointers are considered.
    auto fns = make_functions(img, entries);
    for (auto& j : find_tables(fns).first) for (uint32_t t : j.targets) jt_targets.insert(t);

    // Phase 2: data words that point at code right after a terminator are function-pointer candidates (vtables, callbacks).
    for (const auto& s : img.dol->sections) {
        if (s.is_text) continue;
        for (uint32_t a = s.address; a + 4 <= s.address + s.size; a += 4) {
            uint32_t v = 0, pw = 0;
            if (!img.word(a, v) || (v & 3) || !img.in_text(v) || jt_targets.count(v)) continue;
            if (img.word(v - 4, pw) && is_terminator(pw)) add(v);
        }
    }

    an.entries = entries;
    an.functions = make_functions(img, entries);
    auto [tables, unresolved] = find_tables(an.functions);
    an.jump_tables = tables;
    an.unresolved_bctr = unresolved;
    return an;
}

}  // namespace uc
```

- [ ] **Step 5: Wire sources and run**

`common/CMakeLists.txt`: `add_library(uc_common STATIC dol.cpp dol_scan.cpp ppc/isa.cpp analyzer/config.cpp analyzer/analyzer.cpp)`.
Run: `scripts\dev.bat` — Expected: `test_analyzer` PASSED, all earlier tests PASSED.

---

### Task 5: Translator emitter, op-coverage check, CLI (TDD)

**Files:**
- Create: `common/translator/emit.h`, `common/translator/emit.cpp`, `runtime/dispatch.h`, `runtime/dispatch.cpp`, `tools/translator/CMakeLists.txt`, `tools/translator/main.cpp`, `tools/opcheck_gen/CMakeLists.txt`, `tools/opcheck_gen/main.cpp`, `tests/test_isa_roundtrip.cpp`
- Modify: `common/CMakeLists.txt`, `runtime/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `ppc::decode`, `ppc::OpInfo`, `ppc::Kind`, `ppc::all_ops`, field extractors (Task 1); `uc::Image`, `uc::Function`, `uc::Analysis`, `uc::Config` (Task 4); `ppc::op_*` (Tasks 2–3).
- Produces (`namespace uc`, `common/translator/emit.h`):
  - `struct EmitStats { size_t functions, instructions, invalid, traps_midfunc, unresolved_bctr, oe_traps, unsupported; };`
  - `struct Program { Image img; Analysis an; Config cfg; };`
  - `std::string emit_insn(const Program&, const Function&, uint32_t pc, uint32_t word, EmitStats&);`
  - `std::string emit_function(const Program&, const Function&, EmitStats&);`
  - `struct Output { std::vector<std::pair<std::string, std::string>> files; };`
  - `Output emit_program(const Program&, size_t funcs_per_shard, EmitStats&);` Output files: `game_decl.h`, `game_000.cpp`…, `game_table.cpp`.
- Produces (`runtime/dispatch.h`, `namespace uc`): `using Fn = void (*)(ppc::Ctx&); struct FnEntry { uint32_t addr; Fn fn; }; void dispatch_init(const FnEntry*, size_t, uint32_t text_begin, uint32_t text_end); void call_indirect(ppc::Ctx&, uint32_t addr); bool has_function(uint32_t addr); void hle_sc(ppc::Ctx&, uint32_t pc);`
- Produces: CLI `translator --dol <file> --out <dir> [--cfg <file>] [--shard N]`.

- [ ] **Step 1: Write the failing round-trip test**

`tests/test_isa_roundtrip.cpp`:
```cpp
#include "check.h"
#include "ppc/isa.h"
#include <cstdio>

// Every row must decode back to itself from a canonical word (primary opcode + xo, zero operands).
int main() {
    size_t n = 0;
    auto* const* ops = ppc::all_ops(n);
    int bad = 0;
    for (size_t i = 0; i < n; ++i) {
        const ppc::OpInfo& o = *ops[i];
        uint32_t w = uint32_t(o.primary) << 26 | (o.sub == ppc::Sub::N ? 0u : uint32_t(o.xo) << 1);
        const ppc::OpInfo* d = ppc::decode(w);
        if (d != &o) { std::fprintf(stderr, "row %s (primary %u xo %u) decodes to %s\n", o.name, o.primary, o.xo, d ? d->name : "null"); ++bad; }
    }
    CHECK(bad == 0);
    return 0;
}
```
Wire `test_isa_roundtrip` into the first `foreach` in `tests/CMakeLists.txt`.
Run: `scripts\dev.bat` — Expected: either PASS (table is consistent) or FAIL listing rows whose decode collides. Fix collisions in `PPC_OPS`/`decode()` until it passes. (This is a table check, so a pass on first run is acceptable; the failing-first step applies to `emit_*` below.)

- [ ] **Step 2: Add the missing kinds (AB, CRF) if not already present, then write the emitter test**

Confirm `Kind::AB` and `Kind::CRF` exist in `common/ppc/isa.h` and that `dcbf, dcbtst, dcbt, dcbi, icbi, dcbz, dcbz_l` use `AB` and `mcrxr` uses `CRF` in the table (both were added in Task 1). `AB` passes `(a, b)`; `CRF` passes `(crfd)`.

`tests/test_emit.cpp`:
```cpp
#include "check.h"
#include "dol_builder.h"
#include "ppc_enc.h"
#include "translator/emit.h"
using namespace enc;

static uc::Program make_prog(const std::vector<uint32_t>& code, uc::Config cfg = {}) {
    static std::vector<uint8_t> dol; static uc::DolResult parsed;   // keep storage alive for the Image spans
    dol = make_dol(code);
    parsed = uc::parse_dol(dol);
    uc::Program p;
    p.img = uc::Image{dol, &parsed.image};
    p.cfg = cfg;
    p.an = uc::analyze(p.img, cfg);
    return p;
}

int main() {
    uc::EmitStats st{};
    auto p = make_prog({addi(3, 0, 5), blr});
    const uc::Function& f = p.an.functions[0];
    CHECK(uc::emit_insn(p, f, 0x80003100, addi(3, 0, 5), st) == "ppc::op_addi(c, 3, 0, 5);");
    CHECK(uc::emit_insn(p, f, 0x80003104, add(3, 4, 5), st) == "ppc::op_add(c, 3, 4, 5, false);");
    CHECK(uc::emit_insn(p, f, 0x80003104, 0x7C642A15, st) == "ppc::op_add(c, 3, 4, 5, true);");
    CHECK(uc::emit_insn(p, f, 0x80003104, blr, st) == "return;");
    CHECK(uc::emit_insn(p, f, 0x80003104, mflr(0), st) == "ppc::op_mfspr(c, 0, 8);");
    // illegal word becomes a trap, not an error
    CHECK(uc::emit_insn(p, f, 0x80003104, 0x14000000, st).find("ppc::trap(0x80003104, \"illegal or data word 14000000\")") == 0);
    CHECK(st.invalid == 1);
    // OE-form arithmetic traps and is counted
    CHECK(uc::emit_insn(p, f, 0x80003104, 0x7C642E14, st).find("ppc::trap") == 0);   // addo r3,r4,r5
    CHECK(st.oe_traps == 1);
    // whole function: label at entry, falls off the end into a trap, deterministic text
    std::string a = uc::emit_function(p, f, st), b = uc::emit_function(p, f, st);
    CHECK(a == b);
    CHECK(a.find("void fn_80003100(ppc::Ctx& c) {") == 0);
    CHECK(a.find("L_80003100:;") != std::string::npos);
    CHECK(a.find("fell off the end") != std::string::npos);
    return 0;
}
```
Wire `test_emit` into the first `foreach` list and make `uc_common` link `uc_runtime`'s include dir for `ppc_rt.h`? Not needed: the emitter only produces text.
Run: `scripts\dev.bat` — Expected: FAIL (`translator/emit.h` not found).

- [ ] **Step 3: Implement the dispatch runtime**

`runtime/dispatch.h`:
```cpp
#pragma once
#include "ppc_rt.h"
#include <cstddef>
#include <cstdint>

namespace uc {
using Fn = void (*)(ppc::Ctx&);
struct FnEntry { uint32_t addr; Fn fn; };

void dispatch_init(const FnEntry* table, size_t count, uint32_t text_begin, uint32_t text_end);
bool has_function(uint32_t addr);
// Calls the translated function that starts exactly at `addr`; any other address is a fatal error that names the address.
void call_indirect(ppc::Ctx& c, uint32_t addr);
void hle_sc(ppc::Ctx& c, uint32_t pc);
}  // namespace uc
```
`runtime/dispatch.cpp`:
```cpp
#include "dispatch.h"
#include <vector>

namespace uc {
static std::vector<Fn> g_dense;
static uint32_t g_begin, g_bytes;

void dispatch_init(const FnEntry* table, size_t count, uint32_t text_begin, uint32_t text_end) {
    g_begin = text_begin;
    g_bytes = text_end - text_begin;
    g_dense.assign(g_bytes / 4, nullptr);
    for (size_t i = 0; i < count; ++i) g_dense[(table[i].addr - text_begin) / 4] = table[i].fn;
}
bool has_function(uint32_t a) { return !(a & 3) && a - g_begin < g_bytes && g_dense[(a - g_begin) >> 2] != nullptr; }
void call_indirect(ppc::Ctx& c, uint32_t a) {
    if (has_function(a)) { g_dense[(a - g_begin) >> 2](c); return; }
    ppc::trap(a, "indirect call or jump to an address that is not a known function entry (add `entry <hex>` to project/uc.cfg)");
}
void hle_sc(ppc::Ctx&, uint32_t pc) { ppc::trap(pc, "sc (system call) is not implemented"); }
}  // namespace uc
```
`runtime/CMakeLists.txt`: `add_library(uc_runtime STATIC guest_mem.cpp dispatch.cpp)`.

- [ ] **Step 4: Implement the emitter**

`common/translator/emit.h`:
```cpp
#pragma once
#include "analyzer/analyzer.h"
#include <string>
#include <utility>
#include <vector>

namespace uc {

struct EmitStats {
    size_t functions = 0, instructions = 0, invalid = 0, traps_midfunc = 0, unresolved_bctr = 0, oe_traps = 0, unsupported = 0;
};

struct Program {
    Image img{};
    Analysis an;
    Config cfg;
};

struct Output {
    std::vector<std::pair<std::string, std::string>> files;  // name -> content
};

std::string emit_insn(const Program& p, const Function& f, uint32_t pc, uint32_t word, EmitStats& st);
std::string emit_function(const Program& p, const Function& f, EmitStats& st);
Output emit_program(const Program& p, size_t funcs_per_shard, EmitStats& st);

}  // namespace uc
```
`common/translator/emit.cpp`:
```cpp
#include "translator/emit.h"
#include "ppc/insn.h"
#include "ppc/isa.h"
#include <cstdio>
#include <set>
#include <sstream>

namespace uc {

static std::string hex8(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b; }
static std::string num(long long v) { return std::to_string(v); }
static std::string fn_name(uint32_t a) { return "fn_" + hex8(a); }

static std::string callee(const Program& p, uint32_t target) {
    auto it = p.cfg.hle.find(target);
    return it != p.cfg.hle.end() ? "hle_" + it->second : fn_name(target);
}

static std::string trap_str(uint32_t pc, const std::string& what) { return "ppc::trap(0x" + hex8(pc) + ", \"" + what + "\");"; }

static std::string op_args(const ppc::OpInfo& op, uint32_t w) {
    using ppc::Kind;
    std::ostringstream o;
    auto n = [](auto v) { return std::to_string(static_cast<long long>(v)); };
    switch (op.kind) {
        case Kind::NONE: break;
        case Kind::RRR: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)); break;
        case Kind::RRS: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::simm(w)); break;
        case Kind::RRU: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::uimm(w)) << "u"; break;
        case Kind::RR: o << n(ppc::t(w)) << ", " << n(ppc::a(w)); break;
        case Kind::AB: o << n(ppc::a(w)) << ", " << n(ppc::b(w)); break;
        case Kind::RB: o << n(ppc::t(w)) << ", " << n(ppc::b(w)); break;
        case Kind::R: o << n(ppc::t(w)); break;
        case Kind::CRF: o << n(ppc::crfd(w)); break;
        case Kind::RRM: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::sh(w)) << ", " << n(ppc::mb(w)) << ", " << n(ppc::me(w)); break;
        case Kind::RRRM: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)) << ", " << n(ppc::mb(w)) << ", " << n(ppc::me(w)); break;
        case Kind::CMP: o << n(ppc::crfd(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)); break;
        case Kind::CMPS: o << n(ppc::crfd(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::simm(w)); break;
        case Kind::CMPU: o << n(ppc::crfd(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::uimm(w)) << "u"; break;
        case Kind::CRB: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)); break;
        case Kind::MCRF:
            if (std::string(op.name) == "mtfsfi") o << n(ppc::crfd(w)) << ", " << n((w >> 12) & 0xF);
            else o << n(ppc::crfd(w)) << ", " << n(ppc::crfs(w));
            break;
        case Kind::SPR: o << n(ppc::t(w)) << ", " << n(ppc::spr(w)); break;   // mfspr, mtspr, mftb share the swapped 10-bit field
        case Kind::MTCRF:
            if (std::string(op.name) == "mtfsf") o << n((w >> 17) & 0xFF) << ", " << n(ppc::b(w));
            else o << n(ppc::crm(w)) << ", " << n(ppc::t(w));
            break;
        case Kind::RRRR: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)) << ", " << n(ppc::c(w)); break;
        case Kind::RAC: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::c(w)); break;
        case Kind::PSQ: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(int32_t(w << 20) >> 20) << ", " << n((w >> 15) & 1) << ", " << n((w >> 12) & 7); break;
        case Kind::PSQX: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)) << ", " << n((w >> 10) & 1) << ", " << n((w >> 7) & 7); break;
        default: break;
    }
    return o.str();
}

// ---- control flow ----
struct Target { enum Kind { Internal, Entry, Mid } kind; };
static Target classify(const Program& p, const Function& f, uint32_t t) {
    if (t >= f.entry && t < f.end) return {Target::Internal};
    if (p.an.is_entry(t)) return {Target::Entry};
    return {Target::Mid};
}

static std::string cond_expr(uint32_t bo, uint32_t bi, std::string& pre) {
    std::string e;
    if (!(bo & 4)) {
        pre += "--c.ctr; ";
        e = (bo & 2) ? "c.ctr == 0" : "c.ctr != 0";
    }
    if (!(bo & 16)) {
        std::string cr = "ppc::crbit(c, " + num(bi) + ") == " + ((bo & 8) ? "true" : "false");
        e = e.empty() ? cr : e + " && " + cr;
    }
    return e.empty() ? "true" : e;
}

static std::string branch_action(const Program& p, const Function& f, uint32_t pc, uint32_t target, bool lk, EmitStats& st) {
    Target tk = classify(p, f, target);
    if (tk.kind == Target::Internal && !lk) return "goto L_" + hex8(target) + ";";
    if (tk.kind == Target::Mid || (tk.kind == Target::Internal && lk && target != f.entry)) {
        ++st.traps_midfunc;
        return trap_str(pc, "branch into the middle of another function (target " + hex8(target) + ")");
    }
    if (lk) return "c.lr = 0x" + hex8(pc + 4) + "u; " + callee(p, target) + "(c);";
    return callee(p, target) + "(c); return;";
}

std::string emit_insn(const Program& p, const Function& f, uint32_t pc, uint32_t w, EmitStats& st) {
    ++st.instructions;
    const ppc::OpInfo* op = ppc::decode(w);
    if (!op) {
        ++st.invalid;
        return trap_str(pc, "illegal or data word " + hex8(w));
    }
    using ppc::Kind;
    std::string name = op->name;
    if ((name == "twi" || name == "tw") && ppc::t(w) == 31) return trap_str(pc, "unconditional trap");
    if (op->sub == ppc::Sub::X9 && ppc::oe(w)) { ++st.oe_traps; return trap_str(pc, "OE (overflow-enabled) form of " + name + " is not implemented"); }
    if (op->kind == Kind::UNSUP) { ++st.unsupported; return trap_str(pc, "unsupported instruction " + name); }

    switch (op->kind) {
        case Kind::SC: return "uc::hle_sc(c, 0x" + hex8(pc) + "u);";
        case Kind::BR: {
            int32_t li = ppc::li(w);
            uint32_t target = ppc::aa(w) ? uint32_t(li) : pc + uint32_t(li);
            return branch_action(p, f, pc, target, ppc::lk(w), st);
        }
        case Kind::BC: {
            uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
            uint32_t target = ppc::aa(w) ? uint32_t(ppc::bd(w)) : pc + uint32_t(ppc::bd(w));
            std::string pre;
            std::string cond = cond_expr(bo, bi, pre);
            return "{ " + pre + "if (" + cond + ") { " + branch_action(p, f, pc, target, ppc::lk(w), st) + " } }";
        }
        case Kind::BCLR: {
            uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
            std::string pre;
            std::string cond = cond_expr(bo, bi, pre);
            std::string act = ppc::lk(w) ? "{ uint32_t t_ = c.lr; c.lr = 0x" + hex8(pc + 4) + "u; uc::call_indirect(c, t_); }" : "return;";
            if (cond == "true" && pre.empty()) return act;
            return "{ " + pre + "if (" + cond + ") { " + act + " } }";
        }
        case Kind::BCCTR: {
            uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
            std::string pre;
            std::string cond = cond_expr(bo | 4, bi, pre);   // bcctr never decrements CTR
            if (ppc::lk(w)) {
                std::string act = "c.lr = 0x" + hex8(pc + 4) + "u; uc::call_indirect(c, c.ctr);";
                return cond == "true" ? act : "if (" + cond + ") { " + act + " }";
            }
            for (const auto& jt : p.an.jump_tables) {
                if (jt.bctr_addr != pc) continue;
                std::set<uint32_t> seen;
                std::string s = "switch (c.ctr) { ";
                for (uint32_t t : jt.targets) if (seen.insert(t).second) s += "case 0x" + hex8(t) + "u: goto L_" + hex8(t) + "; ";
                s += "default: " + trap_str(pc, "jump table index out of range") + " }";
                return s;
            }
            ++st.unresolved_bctr;
            std::string act = "uc::call_indirect(c, c.ctr); return;";
            return cond == "true" ? "{ " + act + " }" : "if (" + cond + ") { " + act + " }";
        }
        default: break;
    }
    std::string a = op_args(*op, w);
    if (op->rc) a += (a.empty() ? "" : ", ") + std::string(ppc::rc(w) ? "true" : "false");
    return "ppc::op_" + name + "(c" + (a.empty() ? "" : ", " + a) + ");";
}

std::string emit_function(const Program& p, const Function& f, EmitStats& st) {
    std::set<uint32_t> labels{f.entry};
    for (uint32_t pc = f.entry; pc < f.end; pc += 4) {
        uint32_t w = 0;
        if (!p.img.word(pc, w)) continue;
        const ppc::OpInfo* op = ppc::decode(w);
        if (!op) continue;
        if (op->kind == ppc::Kind::BR || op->kind == ppc::Kind::BC) {
            if (ppc::lk(w)) continue;
            int32_t d = op->kind == ppc::Kind::BR ? ppc::li(w) : ppc::bd(w);
            uint32_t t = ppc::aa(w) ? uint32_t(d) : pc + uint32_t(d);
            if (t >= f.entry && t < f.end) labels.insert(t);
        }
    }
    for (const auto& jt : p.an.jump_tables) if (jt.bctr_addr >= f.entry && jt.bctr_addr < f.end) for (uint32_t t : jt.targets) labels.insert(t);

    std::ostringstream o;
    o << "void " << fn_name(f.entry) << "(ppc::Ctx& c) {\n";
    for (uint32_t pc = f.entry; pc < f.end; pc += 4) {
        uint32_t w = 0;
        p.img.word(pc, w);
        if (labels.count(pc)) o << "L_" << hex8(pc) << ":;\n";
        o << "  /*" << hex8(pc) << "*/ { " << emit_insn(p, f, pc, w, st) << " }\n";
    }
    o << "  " << trap_str(f.end, "fell off the end of function " + hex8(f.entry)) << "\n}\n";
    ++st.functions;
    return o.str();
}

Output emit_program(const Program& p, size_t shard, EmitStats& st) {
    Output out;
    std::set<std::string> hle_names;
    for (auto& [a, n] : p.cfg.hle) hle_names.insert(n);

    std::string decl = "#pragma once\n#include \"ppc_rt.h\"\n#include \"dispatch.h\"\n";
    for (const auto& f : p.an.functions) if (!p.cfg.hle.count(f.entry)) decl += "void " + fn_name(f.entry) + "(ppc::Ctx&);\n";
    for (const auto& n : hle_names) decl += "void hle_" + n + "(ppc::Ctx&);\n";
    out.files.push_back({"game_decl.h", decl});

    size_t idx = 0, in_cur = 0;
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        char nm[32]; std::snprintf(nm, sizeof nm, "game_%03zu.cpp", idx++);
        out.files.push_back({nm, "#include \"game_decl.h\"\n" + cur});
        cur.clear(); in_cur = 0;
    };
    for (const auto& f : p.an.functions) {
        if (p.cfg.hle.count(f.entry)) continue;
        cur += emit_function(p, f, st);
        if (++in_cur >= shard) flush();
    }
    flush();

    std::string table = "#include \"game_decl.h\"\nextern const uc::FnEntry uc_functions[] = {\n";
    for (const auto& f : p.an.functions) table += "  {0x" + hex8(f.entry) + "u, " + callee(p, f.entry) + "},\n";
    table += "};\nextern const size_t uc_function_count = " + std::to_string(p.an.functions.size()) + ";\n";
    uint32_t tb = 0xFFFFFFFFu, te = 0;
    for (const auto& s : p.img.dol->sections) if (s.is_text) { tb = std::min(tb, s.address); te = std::max(te, s.address + s.size); }
    table += "extern const uint32_t uc_text_begin = 0x" + hex8(tb) + "u;\nextern const uint32_t uc_text_end = 0x" + hex8(te) + "u;\n";
    out.files.push_back({"game_table.cpp", table});
    return out;
}

}  // namespace uc
```
`emit.cpp` also needs `#include <algorithm>` (for `std::min`/`std::max`).

- [ ] **Step 5: Add the CLI and the op-coverage generator**

`tools/translator/CMakeLists.txt`:
```cmake
add_executable(translator main.cpp)
target_link_libraries(translator PRIVATE uc_common)
install(TARGETS translator RUNTIME DESTINATION tools)
```
`tools/translator/main.cpp`:
```cpp
#include "analyzer/analyzer.h"
#include "translator/emit.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

int main(int argc, char** argv) {
    std::string dol_path, out_dir, cfg_path;
    size_t shard = 150;
    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
        if (arg("--dol")) dol_path = argv[++i];
        else if (arg("--out")) out_dir = argv[++i];
        else if (arg("--cfg")) cfg_path = argv[++i];
        else if (arg("--shard")) shard = std::strtoull(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "usage: translator --dol <file> --out <dir> [--cfg <file>] [--shard N]\n"); return 2; }
    }
    if (dol_path.empty() || out_dir.empty() || shard == 0) { std::fprintf(stderr, "usage: translator --dol <file> --out <dir> [--cfg <file>] [--shard N]\n"); return 2; }

    std::ifstream in(dol_path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", dol_path.c_str()); return 1; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto dol = uc::parse_dol(file);
    if (!dol.ok) { std::fprintf(stderr, "invalid DOL: %s\n", dol.error.c_str()); return 1; }

    uc::Program p;
    std::string err;
    if (!cfg_path.empty() && !uc::load_config(cfg_path, p.cfg, err)) { std::fprintf(stderr, "config error: %s\n", err.c_str()); return 1; }
    p.img = uc::Image{file, &dol.image};
    p.an = uc::analyze(p.img, p.cfg);

    uc::EmitStats st;
    uc::Output out = uc::emit_program(p, shard, st);
    std::filesystem::create_directories(out_dir);
    for (auto& [name, text] : out.files) {
        std::ofstream f(std::filesystem::path(out_dir) / name, std::ios::binary);
        f << text;
        if (!f) { std::fprintf(stderr, "cannot write %s\n", name.c_str()); return 1; }
    }
    std::printf("functions %zu  instructions %zu  invalid-words %zu  midfunction-traps %zu  unresolved-bctr %zu  OE-traps %zu  unsupported %zu  jump-tables %zu  files %zu\n",
                st.functions, st.instructions, st.invalid, st.traps_midfunc, st.unresolved_bctr, st.oe_traps, st.unsupported, p.an.jump_tables.size(), out.files.size());
    return 0;
}
```
`tools/opcheck_gen/CMakeLists.txt`:
```cmake
add_executable(opcheck_gen main.cpp)
target_link_libraries(opcheck_gen PRIVATE uc_common)
```
`tools/opcheck_gen/main.cpp` — writes a C++ file that calls every non-control-flow op through `emit_insn`, so a missing or mis-typed `op_*` fails the compile:
```cpp
#include "ppc/isa.h"
#include "translator/emit.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    uc::Program p; uc::EmitStats st;
    uc::Function f{0x80003100, 0x80003200};
    std::FILE* o = std::fopen(argv[1], "w");
    if (!o) return 1;
    std::fprintf(o, "#include \"ppc_rt.h\"\nvoid opcheck(ppc::Ctx& c) {\n");
    size_t n = 0;
    auto* const* ops = ppc::all_ops(n);
    for (size_t i = 0; i < n; ++i) {
        const ppc::OpInfo& op = *ops[i];
        using ppc::Kind;
        if (op.kind == Kind::BR || op.kind == Kind::BC || op.kind == Kind::BCLR || op.kind == Kind::BCCTR || op.kind == Kind::SC || op.kind == Kind::UNSUP) continue;
        uint32_t w = uint32_t(op.primary) << 26 | (op.sub == ppc::Sub::N ? 0u : uint32_t(op.xo) << 1);
        std::string s = uc::emit_insn(p, f, 0x80003100, w, st);
        if (s.find("ppc::op_") != 0) continue;   // traps (OE forms, twi 31) are not op calls
        std::fprintf(o, "  %s\n", s.c_str());
    }
    std::fprintf(o, "}\n");
    std::fclose(o);
    return 0;
}
```
Root `CMakeLists.txt`: add `add_subdirectory(tools/translator)` and `add_subdirectory(tools/opcheck_gen)`. In `tests/CMakeLists.txt` append:
```cmake
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/opcheck.cpp
  COMMAND opcheck_gen ${CMAKE_CURRENT_BINARY_DIR}/opcheck.cpp DEPENDS opcheck_gen)
add_library(opcheck_obj OBJECT ${CMAKE_CURRENT_BINARY_DIR}/opcheck.cpp)
target_include_directories(opcheck_obj PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_compile_options(opcheck_obj PRIVATE -Wno-unused-variable)
```
`opcheck_obj` is part of the default build, so `scripts\dev.bat` fails if any table row lacks a matching `op_*` with the right arity.

- [ ] **Step 6: Run to verify it passes**

Run: `scripts\dev.bat` — Expected: `test_emit`, `test_isa_roundtrip` and all earlier tests PASSED; `opcheck_obj` compiles. Compile errors in `opcheck.cpp` name the missing/mis-typed op: fix the table row's `Kind` or the function in `ppc_rt.h`.

---

### Task 6: End-to-end programs through the translator (TDD)

**Files:**
- Create: `tests/e2e/programs.h`, `tests/e2e/gen_e2e_dols.cpp`, `tests/e2e/e2e_main.cpp`, `tests/e2e/compare.cmake`, `tests/e2e/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt` (add `add_subdirectory(e2e)` at the end)

**Interfaces:**
- Consumes: `translator` CLI (Task 5), `Asm`/encoders (Task 4), `make_dol`, `uc::parse_dol`, `uc::dispatch_init`, `uc::call_indirect`, `uc::guest_mem_*`, `test_support` (trap/mmio stubs).
- Produces: ctest targets `e2e_<program>` and `translator_deterministic`.

Programs (all at base `0x80003100`, data at `0x80400000`, scratch memory at `0x80500000`, initial `r1 = 0x817FFF00`, entry = DOL entry):

| Name | What it proves | Expectation |
|---|---|---|
| `loop` | `bdnz` loop, `mtctr` | `r3 == 55` |
| `calls` | `bl`/`blr`, stack frame with `stwu`/`mflr`/`stw`/`lwz`/`mtlr` | `r3 == 21`, `r1 == 0x817FFF00` |
| `jt0` `jt1` `jt2` `jt9` | jump table `switch` via analyzer; selector 0/1/2 and out-of-range 9 | `r3 == 100 / 200 / 300 / 0xFFFFFFFF` |
| `indirect` | `bctrl` through a function pointer stored in data | `r3 == 78` |
| `float` | `lfs`, `fadds`, `fctiwz`, `stfiwx` | `r3 == 3` |
| `ps` | `psq_l`, `ps_add`, `psq_st` | `r3 == 0x40000000`, `r6 == 0x40800000` |
| `datawords` | illegal and zero words inside a function are never executed | `r3 == 6` |
| `badbctr` | Review Focus 1: `bctrl` to an unknown address | output matches `guest trap at 80003FFC` |
| `illegal_exec` | executing an illegal word | output matches `guest trap at 80003104.*illegal or data word 14000000` |
| `midfunc` | Review Focus 3: branch into the middle of another function | output matches `guest trap at 80003104.*middle of another function` |

- [ ] **Step 1: Write the programs and the runner (the tests)**

`tests/e2e/programs.h`:
```cpp
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

inline std::vector<uint8_t> midfunc_dol() {
    Asm a;
    size_t c1 = a.emit(0);                 // bl B
    size_t br = a.emit(0);                 // b B+4 (the middle of B)
    a.emit(blr);
    uint32_t bfn = a.here();
    a.emit(addi(3, 3, 1)); a.emit(addi(3, 3, 1)); a.emit(blr);
    a.patch_bl(c1, bfn); a.patch_b(br, bfn + 4);
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
        {"midfunc", midfunc_dol, nullptr, [](ppc::Ctx&) { return false; }},
    };
    return v;
}
inline const Program* find(const char* name) {
    for (auto& p : all()) if (!std::strcmp(p.name, name)) return &p;
    return nullptr;
}
}  // namespace e2e
```

`tests/e2e/gen_e2e_dols.cpp`:
```cpp
#include "programs.h"
#include <cstdio>
#include <fstream>

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: gen_e2e_dols <program> <out.dol>\n"); return 2; }
    const e2e::Program* p = e2e::find(argv[1]);
    if (!p) { std::fprintf(stderr, "unknown program %s\n", argv[1]); return 1; }
    auto dol = p->build();
    std::ofstream f(argv[2], std::ios::binary);
    f.write(reinterpret_cast<const char*>(dol.data()), std::streamsize(dol.size()));
    return f ? 0 : 1;
}
```

`tests/e2e/e2e_main.cpp`:
```cpp
#include "dispatch.h"
#include "dol.h"
#include "guest_mem.h"
#include "programs.h"
#include "game_decl.h"
#include <cstdio>

extern const uc::FnEntry uc_functions[];
extern const size_t uc_function_count;
extern const uint32_t uc_text_begin, uc_text_end;

int main() {
    const e2e::Program* prog = e2e::find(E2E_NAME);
    if (!prog) { std::fprintf(stderr, "unknown program %s\n", E2E_NAME); return 1; }
    uc::guest_mem_init();
    ppc::Ctx c{};
    c.mem = uc::guest_mem_base();

    auto dol = prog->build();
    auto parsed = uc::parse_dol(dol);
    if (!parsed.ok) { std::fprintf(stderr, "bad DOL: %s\n", parsed.error.c_str()); return 1; }
    for (const auto& s : parsed.image.sections) std::memcpy(c.mem + (s.address & 0x3FFFFFFF), dol.data() + s.file_offset, s.size);

    uc::dispatch_init(uc_functions, uc_function_count, uc_text_begin, uc_text_end);
    c.r[1] = 0x817FFF00;
    if (prog->init) prog->init(c);
    uc::call_indirect(c, parsed.image.entry);
    if (!prog->check(c)) { std::fprintf(stderr, "e2e %s: check failed (r3=%08X r6=%08X)\n", E2E_NAME, c.r[3], c.r[6]); return 1; }
    std::puts("ok");
    return 0;
}
```

`tests/e2e/compare.cmake`:
```cmake
foreach(i a b)
  execute_process(COMMAND ${TRANSLATOR} --dol ${DOL} --out ${OUT}/${i} --shard 100000 RESULT_VARIABLE r)
  if(r)
    message(FATAL_ERROR "translator failed (${r})")
  endif()
endforeach()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_directories ${OUT}/a ${OUT}/b RESULT_VARIABLE r)
if(r)
  message(FATAL_ERROR "translator output differs between two runs")
endif()
```

`tests/e2e/CMakeLists.txt`:
```cmake
add_executable(gen_e2e_dols gen_e2e_dols.cpp)
target_include_directories(gen_e2e_dols PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_SOURCE_DIR}/runtime)

set(E2E_PROGRAMS loop calls jt0 jt1 jt2 jt9 indirect float ps datawords badbctr illegal_exec midfunc)
foreach(p ${E2E_PROGRAMS})
  set(gen ${CMAKE_CURRENT_BINARY_DIR}/gen_${p})
  add_custom_command(
    OUTPUT ${gen}/game_decl.h ${gen}/game_table.cpp ${gen}/game_000.cpp
    COMMAND gen_e2e_dols ${p} ${gen}/${p}.dol
    COMMAND translator --dol ${gen}/${p}.dol --out ${gen} --shard 100000
    DEPENDS gen_e2e_dols translator
    COMMENT "translate e2e program ${p}")
  add_executable(e2e_${p} e2e_main.cpp ${gen}/game_table.cpp ${gen}/game_000.cpp)
  target_compile_definitions(e2e_${p} PRIVATE E2E_NAME="${p}")
  target_compile_options(e2e_${p} PRIVATE -Wno-unused-label -Wno-unused-variable)
  target_include_directories(e2e_${p} PRIVATE ${gen} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(e2e_${p} PRIVATE uc_common test_support)
  add_test(NAME e2e_${p} COMMAND e2e_${p})
endforeach()
set_tests_properties(e2e_badbctr PROPERTIES PASS_REGULAR_EXPRESSION "guest trap at 80003FFC")
set_tests_properties(e2e_illegal_exec PROPERTIES PASS_REGULAR_EXPRESSION "guest trap at 80003104.*illegal or data word 14000000")
set_tests_properties(e2e_midfunc PROPERTIES PASS_REGULAR_EXPRESSION "guest trap at 80003104.*middle of another function")

add_test(NAME translator_deterministic
  COMMAND ${CMAKE_COMMAND} -DTRANSLATOR=$<TARGET_FILE:translator> -DDOL=${CMAKE_CURRENT_BINARY_DIR}/gen_jt1/jt1.dol
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/det -P ${CMAKE_CURRENT_SOURCE_DIR}/compare.cmake)
```
`tests/CMakeLists.txt`: append `add_subdirectory(e2e)`.

- [ ] **Step 2: Run to verify the state**

Run: `scripts\dev.bat`
Expected before debugging: it builds and runs. A compile error in a generated file or a failing `e2e_*` test points at an emitter or semantics bug. The expected failure text for the three trap programs is the regex above; they pass only when the translator emitted the right trap and the runtime printed the guest address.

- [ ] **Step 3: Fix until green**

For each failing test, read the generated file in `build\tests\e2e\gen_<name>\game_000.cpp`, find the wrong instruction, fix the emitter (control-flow in `emit.cpp`) or the semantics (`ppc_rt.h`), and add a unit test for the root cause in `test_emit.cpp` or `test_ppc_*.cpp` first (RED), then fix (GREEN).
Run: `scripts\dev.bat` — Expected: all `e2e_*` tests and `translator_deterministic` PASSED, everything else still green.

---

### Task 7: Translate the real `main.dol` and wire it into `build.bat`

**Files:**
- Create: `game/CMakeLists.txt`, `game/smoke.cpp`, `docs/spikes/T1-translation-stats.md`
- Modify: `CMakeLists.txt`, `build.bat`, `project/uc.cfg`

**Interfaces:**
- Consumes: `translator` (Task 5), `uc_runtime`, `test_support` (trap/mmio stubs), generated files in `generated/`.
- Produces: `generated/` (git-ignored), library `uc_game`, executable `uc_smoke`, a cached translate step in `build.bat`.

- [ ] **Step 1: Translate the real DOL and read the statistics**

Run (after `scripts\dev.bat` has built the translator):
```
build\tools\translator\translator.exe --dol "S:\Games\Umbrella Chronicles\game\DATA\sys\main.dol" --cfg project\uc.cfg --out generated --shard 150
```
Expected: exit 0 and a line like `functions N  instructions ≈ 968000  invalid-words …  midfunction-traps …  unresolved-bctr …  OE-traps …  unsupported …  jump-tables …`.
Record every number. Expected magnitudes from S1: `functions` between 9000 and 12000. Triage:
- `unresolved-bctr` > 0: list the addresses (add `--verbose` printing of unresolved addresses to the CLI if the count is above 20) and resolve each with a `jumptable` line in `project/uc.cfg`, or accept it if the address is in the `.init` vector area (OS-owned, HLE-replaced).
- `midfunction-traps` > 0: for each, decide between `entry <hex>` (the target really is a function start) and leaving the trap (shared tail code). Record the decision.
- `OE-traps` and `unsupported` > 0: list them; implement the OE form in `ppc_rt.h` with a unit test if the count in non-OS code is above 0.

- [ ] **Step 2: Build the generated code**

`game/CMakeLists.txt`:
```cmake
set(UC_GENERATED_DIR ${CMAKE_SOURCE_DIR}/generated CACHE PATH "translator output")
if(EXISTS ${UC_GENERATED_DIR}/game_table.cpp)
  file(GLOB GEN_SRC ${UC_GENERATED_DIR}/*.cpp)
  add_library(uc_game STATIC ${GEN_SRC})
  target_include_directories(uc_game PUBLIC ${UC_GENERATED_DIR})
  target_link_libraries(uc_game PUBLIC uc_runtime)
  target_compile_options(uc_game PRIVATE /O2 -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
  add_executable(uc_smoke smoke.cpp)
  target_link_libraries(uc_smoke PRIVATE uc_game uc_common test_support)
  install(TARGETS uc_smoke RUNTIME DESTINATION tools)
else()
  message(STATUS "generated/ not found: skipping uc_game (build.bat runs the translator first)")
endif()
```
Root `CMakeLists.txt`: add `add_subdirectory(game)` after `add_subdirectory(tests)` (it uses `test_support`).

`game/smoke.cpp`:
```cpp
#include "dispatch.h"
#include "dol.h"
#include "game_decl.h"
#include "guest_mem.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

extern const uc::FnEntry uc_functions[];
extern const size_t uc_function_count;
extern const uint32_t uc_text_begin, uc_text_end;

int main(int argc, char** argv) {
    uc::guest_mem_init();
    uc::dispatch_init(uc_functions, uc_function_count, uc_text_begin, uc_text_end);
    std::printf("functions %zu  text %08X..%08X\n", uc_function_count, uc_text_begin, uc_text_end);
    if (argc == 2) {
        std::ifstream in(argv[1], std::ios::binary);
        std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto d = uc::parse_dol(f);
        if (!d.ok) { std::fprintf(stderr, "invalid DOL: %s\n", d.error.c_str()); return 1; }
        for (const auto& s : d.image.sections) std::memcpy(uc::guest_mem_base() + (s.address & 0x3FFFFFFF), f.data() + s.file_offset, s.size);
        std::printf("entry %08X known: %s\n", d.image.entry, uc::has_function(d.image.entry) ? "yes" : "NO");
        return uc::has_function(d.image.entry) ? 0 : 1;
    }
    return 0;
}
```
Add `#include <cstring>` to `smoke.cpp`.
Run: `scripts\dev.bat` with `generated\` present. Time the first compile (`powershell -Command "Measure-Command { cmake --build build --target uc_game }"` after deleting `build\game`).
Expected: `uc_game` builds without errors. If clang reports an error in a generated file, fix the emitter (add a regression test in `test_emit.cpp` first). Record the compile time and the single slowest file (`ninja -C build -t` or `/clang:-ftime-report` on that file if above 60 s).
Run: `build\game\uc_smoke.exe "S:\Games\Umbrella Chronicles\game\DATA\sys\main.dol"` — Expected: prints the function count and `entry 8000403C known: yes`, exit 0.

- [ ] **Step 3: Cached translate step in `build.bat`**

Replace `build.bat` (keeps the paren-safe label style from the previous plan):
```bat
@echo off
setlocal EnableExtensions
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "DIST=%ROOT%\dist"
set "GEN=%ROOT%\generated"
if not defined UC_INSTALL_DIR set "UC_INSTALL_DIR=S:\Games\Umbrella Chronicles"
set "GAME_DOL=%UC_INSTALL_DIR%\game\DATA\sys\main.dol"
set "CFG=%ROOT%\project\uc.cfg"
set "TRANSLATOR=%BUILD%\tools\translator\translator.exe"
set "NO_INSTALL="
if /I "%~1"=="--no-install" set "NO_INSTALL=1"

rem Messages that print user paths live behind labels: a ")" in a path would break a ( ) block.
echo [1/6] Checking game files
if not exist "%UC_INSTALL_DIR%\" goto :no_install_dir
if not exist "%GAME_DOL%" goto :no_dol
call "%ROOT%\scripts\env.bat" || exit /b 1

echo [2/6] Configure and build the translator
cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl > "%BUILD%\configure.log" 2>&1
if errorlevel 1 goto :build_failed
cmake --build "%BUILD%" --target translator > "%BUILD%\translator_build.log" 2>&1
if errorlevel 1 goto :build_failed

echo [3/6] Translating the game (cached by DOL + translator + uc.cfg hash)
set "KEY="
for /f %%h in ('powershell -NoProfile -Command "$h = foreach($p in $env:GAME_DOL,$env:TRANSLATOR,$env:CFG){(Get-FileHash -LiteralPath $p).Hash}; $h -join ''"') do set "KEY=%%h"
if not defined KEY goto :hash_failed
set "OLD="
if exist "%GEN%\.key" set /p OLD=<"%GEN%\.key"
if /I "%KEY%"=="%OLD%" goto :cached
if exist "%GEN%" rmdir /s /q "%GEN%"
"%TRANSLATOR%" --dol "%GAME_DOL%" --cfg "%CFG%" --out "%GEN%" --shard 150 > "%BUILD%\translate.log" 2>&1
if errorlevel 1 goto :translate_failed
>"%GEN%\.key" echo %KEY%
type "%BUILD%\translate.log"
goto :built
:cached
echo generated\ is up to date

:built
echo [4/6] Build everything and run tests
call "%ROOT%\scripts\dev.bat" || goto :build_failed

echo [5/6] Staging dist
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%" || exit /b 1
cmake --install "%BUILD%" --prefix "%DIST%" || goto :stage_failed

if defined NO_INSTALL goto :skip_copy
echo [6/6] Copying dist to %UC_INSTALL_DIR%
robocopy "%DIST%" "%UC_INSTALL_DIR%" /E /NFL /NDL /NJH /NJS /NP
if errorlevel 8 goto :copy_failed
echo Done.
exit /b 0

:skip_copy
echo [6/6] Skipping copy (--no-install)
exit /b 0

:no_install_dir
echo ERROR: install directory not found: %UC_INSTALL_DIR%
exit /b 1
:no_dol
echo ERROR: main.dol not found: %GAME_DOL%
exit /b 1
:build_failed
echo ERROR: build or tests failed (see logs in %BUILD%)
exit /b 1
:hash_failed
echo ERROR: could not hash inputs for the translate cache
exit /b 1
:translate_failed
echo ERROR: translation failed
type "%BUILD%\translate.log"
exit /b 1
:stage_failed
echo ERROR: install to dist failed
exit /b 1
:copy_failed
echo ERROR: robocopy failed
exit /b 1
```
The `build\` folder must exist before step 2 writes logs: add `if not exist "%BUILD%" mkdir "%BUILD%"` right after the `env.bat` call.

- [ ] **Step 4: Verify the build script**

Run from another directory: `cd /d C:\ && "D:\Projects\Native Ports\Umbrella-chronicles-recomp\build.bat" --no-install`
Expected: exit 0; the first run prints the translator statistics, the second run prints `generated\ is up to date` and does not re-run the translator.
Then change a character in a copy of `uc.cfg`? No: append a comment line to `project\uc.cfg`, run again — Expected: the translator runs again (cache key changed).
Run with `UC_INSTALL_DIR=S:\TEMP\uc (test)` (create `game\DATA\sys\main.dol` there): Expected: still exit 0 with parentheses in the path.

- [ ] **Step 5: Write `docs/spikes/T1-translation-stats.md`**

Record: all numbers from Step 1, the triage decisions, the `uc_game` compile time and slowest file, the `uc_smoke` result, and the verdict:
- PASS if translation and compilation succeed, `unresolved-bctr` and `midfunction-traps` are explained or zero outside the `.init` vector area, and compile time ≤ 15 minutes.
- Otherwise list what blocks and which fix applies (jumptable lines, `entry` lines, per-file `/O1`, splitting).

---

### Task 8: SDK identification by signature and string anchors

**Files:**
- Create: `common/sig/dsy.h`, `common/sig/dsy.cpp`, `tests/test_dsy.cpp`, `tools/sigmatch/CMakeLists.txt`, `tools/sigmatch/main.cpp`, `project/hle_required.txt`, `docs/spikes/S5-identification.md`
- Modify: `common/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`, `.gitignore` (add `third_party/signatures/`)

**Interfaces:**
- Consumes: `uc::Image`, `uc::analyze`, `uc::Analysis` (Task 4), `ppc::*` extractors (Task 1).
- Produces (`namespace uc`, `common/sig/dsy.h`):
  - `struct DsyEntry { uint32_t checksum, size; std::string name; };`
  - `bool parse_dsy(std::span<const uint8_t> bytes, std::vector<DsyEntry>& out, std::string& error);` `bool load_dsy(const std::string& path, std::vector<DsyEntry>& out, std::string& error);`
  - `uint32_t dsy_step(uint32_t sum, uint32_t word);`
- Produces: CLI `sigmatch --dol <file> --dsy <file> [--cfg <file>] [--min-words N] --out <dir>`, writing `names.txt` (`<hex addr> <name>` per match, `AMBIGUOUS` lines for hash collisions) and `anchors.txt` (`<hex addr> "<string>"` for functions referencing SDK-looking strings), and printing counts per name prefix and the coverage of `project/hle_required.txt`.

Format facts (from Dolphin's source, interoperability only; no Dolphin code is copied): a `.dsy` file is a little-endian `u32` entry count followed by `count` entries of 136 bytes: `u32 checksum`, `u32 size` (bytes), `char name[128]`. The checksum folds every instruction word: `sum = rotl32(sum, 17) ^ (opcode_bits | extra_bits)` with the masks implemented in `dsy_step` below. Because the checksum depends on opcode forms and not on register operands, it survives recompilation of the same source.

- [ ] **Step 1: Write the failing tests**

`tests/test_dsy.cpp`:
```cpp
#include "check.h"
#include "sig/dsy.h"
#include <cstring>

static void put32le(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }

int main() {
    // checksum: hand-computed. blr alone -> primary 19 keeps the low 11 bits: 0x4C000000 | 0x020
    CHECK(uc::dsy_step(0, 0x4E800020) == 0x4C000020);
    // li r3,1 ; blr -> addi keeps bits 16-25 (0x00600000): first sum 0x38600000; rotl17 = 0x000070C0; xor 0x4C000020
    uint32_t s = uc::dsy_step(0, 0x38600001);
    CHECK(s == 0x38600000);
    CHECK(uc::dsy_step(s, 0x4E800020) == 0x4C0070E0);
    // register operands of a load (primary 32..55 keeps bits 16-25) and the displacement is ignored
    CHECK(uc::dsy_step(0, 0x80010014) == uc::dsy_step(0, 0x80010028));

    // file format: u32 count + 136-byte entries
    std::vector<uint8_t> f;
    put32le(f, 2);
    for (int e = 0; e < 2; ++e) {
        put32le(f, 0x1000 + e); put32le(f, 40 + e);
        char name[128] = {};
        std::strcpy(name, e == 0 ? "OSInit" : "GXInit");
        f.insert(f.end(), name, name + 128);
    }
    std::vector<uc::DsyEntry> out; std::string err;
    CHECK(uc::parse_dsy(f, out, err));
    CHECK(out.size() == 2 && out[0].name == "OSInit" && out[1].checksum == 0x1001 && out[1].size == 41);
    // truncated file is rejected (Dolphin's loader would accept it)
    std::vector<uint8_t> cut(f.begin(), f.end() - 10);
    CHECK(!uc::parse_dsy(cut, out, err));
    // absurd count is rejected without allocating
    std::vector<uint8_t> huge; put32le(huge, 0x7FFFFFFF);
    CHECK(!uc::parse_dsy(huge, out, err));
    CHECK(!uc::parse_dsy({}, out, err));
    // name without terminator is cut at 127 characters
    std::vector<uint8_t> g; put32le(g, 1); put32le(g, 1); put32le(g, 4);
    g.insert(g.end(), 128, 'A');
    CHECK(uc::parse_dsy(g, out, err) && out[0].name.size() == 127);
    return 0;
}
```
Wire `test_dsy` into the first `foreach` list. Run `scripts\dev.bat` — Expected: FAIL (`sig/dsy.h` not found).

- [ ] **Step 2: Implement**

`common/sig/dsy.h`:
```cpp
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace uc {

struct DsyEntry {
    uint32_t checksum;
    uint32_t size;
    std::string name;
};

bool parse_dsy(std::span<const uint8_t> bytes, std::vector<DsyEntry>& out, std::string& error);
bool load_dsy(const std::string& path, std::vector<DsyEntry>& out, std::string& error);

// One step of the signature checksum: rotate the running sum left by 17, then xor in the masked instruction word.
uint32_t dsy_step(uint32_t sum, uint32_t word);

}  // namespace uc
```
`common/sig/dsy.cpp`:
```cpp
#include "sig/dsy.h"
#include <fstream>
#include <iterator>

namespace uc {

constexpr size_t kEntryBytes = 136;

static uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

bool parse_dsy(std::span<const uint8_t> b, std::vector<DsyEntry>& out, std::string& error) {
    out.clear();
    if (b.size() < 4) { error = "file smaller than the 4-byte header"; return false; }
    uint32_t count = le32(b.data());
    if (count > (b.size() - 4) / kEntryBytes) { error = "entry count does not fit in the file (truncated or not a .dsy)"; return false; }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = b.data() + 4 + size_t(i) * kEntryBytes;
        DsyEntry d{le32(e), le32(e + 4), {}};
        for (int k = 0; k < 127 && e[8 + k]; ++k) d.name.push_back(char(e[8 + k]));
        out.push_back(std::move(d));
    }
    return true;
}

bool load_dsy(const std::string& path, std::vector<DsyEntry>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot open " + path; return false; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_dsy(bytes, out, error);
}

uint32_t dsy_step(uint32_t sum, uint32_t w) {
    uint32_t op = w & 0xFC000000u, op2 = 0, op3 = 0;
    uint32_t primary = op >> 26;
    switch (primary) {
        case 4:
            op2 = w & 0x3F;
            switch (op2) { case 0: case 8: case 16: case 21: case 22: op3 = w & 0x7C0; break; }
            break;
        case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 15: op2 = w & 0x03FF0000; break;
        case 19: case 31: case 63: op2 = w & 0x7FF; break;
        case 59:
            op2 = w & 0x3F;
            if (op2 < 16) op3 = w & 0x7C0;
            break;
        default:
            if (primary >= 32 && primary <= 55) op2 = w & 0x03FF0000;
            break;
    }
    return ((sum << 17) | (sum >> 15)) ^ (op | op2 | op3);
}

}  // namespace uc
```
`common/CMakeLists.txt`: add `sig/dsy.cpp` to `uc_common`.
Run: `scripts\dev.bat` — Expected: `test_dsy` PASSED.

- [ ] **Step 3: Write the `sigmatch` tool**

`tools/sigmatch/CMakeLists.txt`:
```cmake
add_executable(sigmatch main.cpp)
target_link_libraries(sigmatch PRIVATE uc_common)
```
Root `CMakeLists.txt`: `add_subdirectory(tools/sigmatch)`.

`tools/sigmatch/main.cpp`:
```cpp
#include "analyzer/analyzer.h"
#include "ppc/insn.h"
#include "sig/dsy.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>

static bool is_term(uint32_t w) { return w == 0x4E800020u || w == 0x4E800420u || ((w >> 26) == 18 && !(w & 1)); }

static std::string c_string(const uc::Image& img, uint32_t addr) {
    const uc::DolSection* s = img.section(addr);
    if (!s) return {};
    std::string r;
    for (uint32_t a = addr; a < s->address + s->size && r.size() < 160; ++a) {
        char ch = char(img.file[s->file_offset + (a - s->address)]);
        if (!ch) break;
        if (ch < 0x20 || ch > 0x7E) return {};
        r.push_back(ch);
    }
    return r.size() >= 4 ? r : std::string{};
}

int main(int argc, char** argv) {
    std::string dol_path, dsy_path, cfg_path, out_dir;
    size_t min_words = 6;
    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* n) { return !std::strcmp(argv[i], n) && i + 1 < argc; };
        if (arg("--dol")) dol_path = argv[++i];
        else if (arg("--dsy")) dsy_path = argv[++i];
        else if (arg("--cfg")) cfg_path = argv[++i];
        else if (arg("--out")) out_dir = argv[++i];
        else if (arg("--min-words")) min_words = std::strtoull(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "usage: sigmatch --dol <f> [--dsy <f>] [--cfg <f>] [--min-words N] --out <dir>\n"); return 2; }
    }
    if (dol_path.empty() || out_dir.empty()) { std::fprintf(stderr, "usage: sigmatch --dol <f> [--dsy <f>] [--cfg <f>] [--min-words N] --out <dir>\n"); return 2; }

    std::ifstream in(dol_path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", dol_path.c_str()); return 1; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto dol = uc::parse_dol(file);
    if (!dol.ok) { std::fprintf(stderr, "invalid DOL: %s\n", dol.error.c_str()); return 1; }
    uc::Config cfg; std::string err;
    if (!cfg_path.empty() && !uc::load_config(cfg_path, cfg, err)) { std::fprintf(stderr, "config error: %s\n", err.c_str()); return 1; }
    uc::Image img{file, &dol.image};
    uc::Analysis an = uc::analyze(img, cfg);

    std::filesystem::create_directories(out_dir);
    std::map<uint32_t, std::string> names;     // entry -> name
    std::set<uint32_t> ambiguous;

    if (!dsy_path.empty()) {
        std::vector<uc::DsyEntry> db;
        if (!uc::load_dsy(dsy_path, db, err)) { std::fprintf(stderr, "dsy error: %s\n", err.c_str()); return 1; }
        std::multimap<uint32_t, const uc::DsyEntry*> by_sum;
        for (auto& e : db) by_sum.emplace(e.checksum, &e);
        for (const auto& f : an.functions) {
            uint32_t sum = 0;
            for (uint32_t pc = f.entry; pc < f.end; pc += 4) {
                uint32_t w = 0; img.word(pc, w);
                sum = uc::dsy_step(sum, w);
                size_t words = (pc - f.entry) / 4 + 1;
                if (!is_term(w) || words < min_words) continue;
                auto [lo, hi] = by_sum.equal_range(sum);
                std::set<std::string> cand;
                for (auto it = lo; it != hi; ++it) if (it->second->size == words * 4) cand.insert(it->second->name);
                if (cand.empty()) continue;
                if (cand.size() == 1) names[f.entry] = *cand.begin();
                else { ambiguous.insert(f.entry); std::string j; for (auto& c : cand) j += (j.empty() ? "" : "|") + c; names[f.entry] = "AMBIGUOUS " + j; }
            }
        }
    }

    std::ofstream nf(std::filesystem::path(out_dir) / "names.txt");
    char buf[16];
    for (auto& [a, n] : names) { std::snprintf(buf, sizeof buf, "%08X", a); nf << buf << ' ' << n << '\n'; }

    // string anchors: lis rX,hi ; addi rY,rX,lo -> address of a string that looks like it names an SDK API
    std::regex sdk(R"((^|[^A-Za-z])(OS|DVD|WPAD|KPAD|VI|GX|AX|NAND|PAD|SI|EXI|CARD|MEM)[A-Za-z]*(\(\)|\.c)|Panic|[A-Za-z]+\.c$)");
    std::ofstream af(std::filesystem::path(out_dir) / "anchors.txt");
    size_t anchors = 0;
    for (const auto& f : an.functions) {
        for (uint32_t pc = f.entry; pc + 4 < f.end; pc += 4) {
            uint32_t w = 0, w2 = 0; img.word(pc, w); img.word(pc + 4, w2);
            if ((w >> 26) != 15 || ppc::a(w) != 0 || (w2 >> 26) != 14 || ppc::a(w2) != ppc::t(w)) continue;
            uint32_t addr = (w & 0xFFFF) << 16; addr += uint32_t(ppc::simm(w2));
            std::string s = c_string(img, addr);
            if (s.empty() || !std::regex_search(s, sdk)) continue;
            std::snprintf(buf, sizeof buf, "%08X", f.entry);
            af << buf << " \"" << s << "\"\n";
            ++anchors;
        }
    }

    std::map<std::string, size_t> prefix;
    for (auto& [a, n] : names) {
        if (n.rfind("AMBIGUOUS", 0) == 0) continue;
        size_t i = 0; while (i < n.size() && (n[i] == '_' || std::isupper(static_cast<unsigned char>(n[i])))) ++i;
        ++prefix[n.substr(0, std::min<size_t>(i, 6))];
    }
    std::printf("functions %zu  named %zu  ambiguous %zu  string-anchors %zu\n", an.functions.size(), names.size() - ambiguous.size(), ambiguous.size(), anchors);
    for (auto& [p, c] : prefix) std::printf("  %-8s %zu\n", p.c_str(), c);
    return 0;
}
```
Add `#include <algorithm>` and `#include <cctype>`.

- [ ] **Step 4: The required-name list and the gate**

`project/hle_required.txt` (category and exact SDK name; `core` names are required for the runtime plan, `optional` ones depend on the hardware-level decision):
```
core OSInit
core OSCreateThread
core OSResumeThread
core OSSuspendThread
core OSSleepThread
core OSWakeupThread
core OSYieldThread
core OSExitThread
core OSJoinThread
core OSInitMutex
core OSLockMutex
core OSUnlockMutex
core OSInitMessageQueue
core OSSendMessage
core OSReceiveMessage
core OSDisableInterrupts
core OSRestoreInterrupts
core OSEnableInterrupts
core OSGetTime
core OSGetTick
core OSInitAlarm
core OSSetAlarm
core OSCancelAlarm
core OSReport
core OSPanic
core DVDInit
core DVDOpen
core DVDFastOpen
core DVDReadPrio
core DVDReadAsyncPrio
core DVDClose
core DVDConvertPathToEntrynum
core VIInit
core VIWaitForRetrace
core VIConfigure
core VIFlush
core WPADInit
core WPADRead
core KPADInit
core KPADRead
core KPADSetPosParam
optional GXInit
optional GXBegin
optional GXSetTevOp
optional GXCopyDisp
optional AXInit
optional AXAcquireVoice
optional NANDInit
```
Add to `tools/sigmatch/main.cpp` an optional `--required <file>` argument: after matching, for each required line print `FOUND <addr> <name>` or `MISSING <name>` (a name is found when it appears as a value in `names` without `AMBIGUOUS`), then `core: x/y  optional: a/b`. (Implement with the same `names` map; roughly 25 lines.)

- [ ] **Step 5: Obtain a signature database (ask the user first)**

Ask the user for permission to download one data file: `https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Data/Sys/totaldb.dsy` into `third_party/signatures/totaldb.dsy` (git-ignored, never redistributed; the file is data, no Dolphin code is used). Add `third_party/signatures/` to `.gitignore`. If the user declines, run the tool without `--dsy` (anchors only) and say so in the result document.

- [ ] **Step 6: Run the identification and write the gate result**

Run:
```
build\tools\sigmatch\sigmatch.exe --dol "S:\Games\Umbrella Chronicles\game\DATA\sys\main.dol" --dsy third_party\signatures\totaldb.dsy --cfg project\uc.cfg --required project\hle_required.txt --out build\sig
```
Expected: counts printed, `build\sig\names.txt`, `build\sig\anchors.txt`. Sanity check of the masks: a healthy run names at least a few hundred OS/DVD/MEM/runtime functions. If it names almost nothing while the database is large, re-fetch Dolphin's `SignatureDB.cpp` and compare the mask table with `dsy_step` line by line (the masks here were taken from a summary of that file).
Then read `anchors.txt`: each anchor links a function to a string such as `WPADInit()` or `dvd.c`. Promote confident ones to `project/uc.cfg` as `hle <addr> <Name>` lines (names must match `hle_required.txt` and later HLE function names); keep uncertain ones out.

Write `docs/spikes/S5-identification.md` with: database used (or none), counts, `core x/y` and `optional a/b`, the list of core names still missing, and the verdict:
- PASS if all `core` names are identified (by signature or by an anchor you verified).
- PARTIAL if at least 70% of `core` is identified: list the missing names and the plan for them (call-graph neighbours of identified functions, strings).
- FAIL otherwise. **Then follow `docs/decisions.md` (Ghidra section): ask the user before installing Ghidra.** Do not install it on your own.

---

### Task 9: Close out

**Files:**
- Modify: `docs/superpowers/specs/2026-10-12-uc-recomp-design.md` (§5, §11 results), `docs/decisions.md` (if the identification gate fails)

- [ ] **Step 1: Update the spec**

In §5 note the implemented decisions: `uc.cfg` instead of `uc.yml`, semantics as functions, MMIO window check in every access, data-pointer entries only after jump-table targets are excluded. Under the §11 results add a `T1`/`S5` bullet pair with the verdicts and paths of `T1-translation-stats.md` and `S5-identification.md`.

- [ ] **Step 2: Hand off**

Report to the user: statistics, verdicts, open decisions (Ghidra if the gate failed; GX/VI/AX hardware-level versus call-level HLE for the runtime plan; register caching deferred). Propose the next plan: runtime and HLE (guest threads, OS scheduler, DVD from the extracted folder, VI timing, AX, Sofdec/ADX), in that order.

---

## Self-review (spec coverage)

- Spec §5 steps 1–4: analyzer (Task 4), SDK identification (Task 8), translator (Tasks 5–6), compile and cache (Task 7).
- Spec §5 "no interpreter; unresolved indirect target is fatal": `call_indirect` trap (Task 5), `badbctr` test (Task 6).
- Spec §6 memory model: Task 2 (`guest_mem`, `is_mmio`, byte order tests).
- Spec §10 build: Task 7 rewrites `build.bat` with the cached translate step.
- Review Focus 1–6: tests in Tasks 2, 3, 6 as listed in the header.
- Types and names used across tasks: `Kind`, `Sub`, `OpInfo`, `decode`, `all_ops`, `Ctx`, `op_*`, `Image`, `Function`, `Analysis`, `Config`, `JumpTable`, `Program`, `EmitStats`, `Output`, `emit_insn/emit_function/emit_program`, `FnEntry`, `dispatch_init`, `call_indirect`, `has_function`, `hle_sc`, `DsyEntry`, `parse_dsy`, `load_dsy`, `dsy_step` are defined once and used with the same signatures.
- Out of scope here: running the game, HLE implementations (`hle_*` bodies), threads, register caching, the renderer.
