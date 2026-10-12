#include "ppc_rt.h"
// EXI bus with no device attached (tech-debt TD-010). Calls are replaced at the SDK level; no EXI register is ever driven.
using namespace ppc;
static void zero_fill(Ctx& c, uint32_t buf, uint32_t len) { for (uint32_t i = 0; i < len; ++i) wr8(c, buf + i, 0); }
static void transfer(Ctx& c, uint32_t buf, uint32_t len, uint32_t type) { if (type != 1) zero_fill(c, buf, len); c.r[3] = 1; }   // type 1 = write only
void hle_EXIImm(Ctx& c) { transfer(c, c.r[4], c.r[5], c.r[6]); }
void hle_EXIDma(Ctx& c) { transfer(c, c.r[4], c.r[5], c.r[6]); }
void hle_EXIImmEx(Ctx& c) { transfer(c, c.r[4], c.r[5], c.r[6]); }
void hle_EXISync(Ctx& c) { c.r[3] = 1; }
void hle_EXISelect(Ctx& c) { c.r[3] = 1; }
void hle_EXIDeselect(Ctx& c) { c.r[3] = 1; }
void hle_EXILock(Ctx& c) { c.r[3] = 1; }
void hle_EXIUnlock(Ctx& c) { c.r[3] = 1; }
void hle_EXIGetID(Ctx& c) { c.r[3] = 0; }
void hle_EXIAttach(Ctx& c) { c.r[3] = 0; }
void hle_EXIDetach(Ctx& c) { c.r[3] = 1; }
void hle_EXIProbe(Ctx& c) { c.r[3] = 0; }
