#pragma once
#include "ppc_rt.h"
#include "sched.h"
#include <string>
namespace uc {
void os_hle_init(Sched& s, uint8_t* guest_mem, uint32_t dummy_context_addr);   // dummy_context_addr: guest memory handed to alarm handlers as their OSContext*
uint32_t os_dummy_context();
void os_log_sleep(bool on);   // print the guest trace at every OSSleepThread (bring-up diagnostic)
Sched& os_sched();
std::string format_guest(ppc::Ctx& c, uint32_t fmt_addr, int first_gpr);
}
