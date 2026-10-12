#include "ppc_rt.h"
#include "gx_audit.h"
void hle_Nop(ppc::Ctx&) {}
void hle_Return0(ppc::Ctx& c) { c.r[3] = 0; }
void hle_Return1(ppc::Ctx& c) { c.r[3] = 1; }
