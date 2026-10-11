#pragma once
#include <cstdint>
namespace uc {
void guest_mem_init();          // idempotent; reserves 1 GB, commits MEM1 (24 MB) and MEM2 (64 MB)
uint8_t* guest_mem_base();
}
