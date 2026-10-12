#pragma once
#include "ppc_rt.h"
#include <initializer_list>
namespace uc {
// Calls the translated function at `addr` with integer arguments in r3..; returns r3. LR is preserved.
uint32_t call_guest(ppc::Ctx& c, uint32_t addr, std::initializer_list<uint32_t> args = {});
}
