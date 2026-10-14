#pragma once
#include "gx_packets.h"
#include "sched.h"
namespace uc {
struct GxStats { uint64_t draws = 0, vertices = 0, submitted = 0, completed = 0, outside_bytes = 0, copies = 0, texture_decodes = 0, geometry_hits = 0, geometry_misses = 0, prompt_annotations=0,prompt_present=0,prompt_fallback=0; };
void gx_hle_init(Sched& sched, uint8_t* memory, gx::Packets* packets, bool profile=false);
void gx_fifo_write(int size, uint32_t value); // resource recording writes bytes; live primitives consume descriptor-sized attributes
void gx_completed(uint64_t serial);          // any host thread; callback runs later on the guest baton
GxStats gx_stats();
bool gx_fifo_in_primitive(); // guest bytes are vertex data of an open primitive
bool gx_fifo_recording();
struct GxPromptCheckpoint {uint64_t batch;size_t draw;uint32_t producer,list;size_t recorded;};
GxPromptCheckpoint gx_prompt_checkpoint();
// Replaces the single SDK quad drawn since the checkpoint (live, or recorded into a display list) with a port prompt.
bool gx_prompt_attach(GxPromptCheckpoint,const PromptDescription&,bool compact=false);
}
