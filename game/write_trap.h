#pragma once
#include "ppc_rt.h"
namespace uc {
// Diagnostic: UC_WRITE_TRAP=<report>|<hex guest address>[,<hex guest address>...]. Once <report>.go exists the pages holding those words become read-only; the first
// writes of each distinct host instruction are logged to <report> (host RVA, written address, thread) and execution continues. Resolve the RVAs with uc.map.
void write_trap_step(const ppc::Ctx&);
}
