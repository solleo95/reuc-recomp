#pragma once
#include "ppc_rt.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace uc {
using Fn = void (*)(ppc::Ctx&);
struct FnEntry { uint32_t addr; Fn fn; };

void dispatch_init(const FnEntry* table, size_t count, uint32_t text_begin, uint32_t text_end);
bool has_function(uint32_t addr);
std::vector<uint32_t> function_addresses();   // entry of every translated function, ascending
// Calls the translated function that starts exactly at `addr`; any other address is a fatal error that names the address.
void call_indirect(ppc::Ctx& c, uint32_t addr);
void hle_sc(ppc::Ctx& c, uint32_t pc);
// Guest entry of the translated (or HLE) function containing host address `pc`; 0 outside generated code.
uint32_t guest_function_for_host(const void* pc);
}  // namespace uc
