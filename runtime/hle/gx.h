#pragma once
#include "gx_packets.h"
#include "sched.h"
namespace uc {
struct GxStats { uint64_t draws = 0, vertices = 0, submitted = 0, completed = 0, outside_bytes = 0, copies = 0; };
void gx_hle_init(Sched& sched, uint8_t* memory, gx::Packets* packets);
void gx_fifo_write(int size, uint32_t value); // resource recording writes bytes; live primitives consume descriptor-sized attributes
void gx_completed(uint64_t serial);          // any host thread; callback runs later on the guest baton
GxStats gx_stats();
}
