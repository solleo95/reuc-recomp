#include "ppc_rt.h"
#include "gx_audit.h"
void hle_Nop(ppc::Ctx&) {}
void hle_Return0(ppc::Ctx& c) { c.r[3] = 0; }
void hle_Return1(ppc::Ctx& c) { c.r[3] = 1; }
void hle_KPADRead(ppc::Ctx& c) { uc::gx_audit_enter(0x802F21EC,c); c.r[3] = 0; } // existing TD-013; observe real input polling before the native input bridge
