#pragma once
#include "ppc_rt.h"
namespace uc {
// Diagnostic for frame-rate dependence: UC_STATE_DUMP=<prefix>|<seconds>,<seconds>,... Once <prefix>.go exists it adds up the game time of every step and writes the whole
// guest RAM (MEM1 then MEM2) to <prefix>_<seconds>.bin at the first step that reaches each listed time. Run the same scripted scene at two step rates and compare the
// snapshots word by word (tools/spikes/state_diff.py): what moved the same distance at the same game time follows the real step, the rest counts calls.
void state_dump_step(const ppc::Ctx&, float timestep);
// Diagnostic (builds with -DUC_FN_COUNT=ON): UC_FN_COUNT=<prefix> counts the entries of every translated function; creating <prefix>.snap makes the next step write
// <prefix>_<n>.txt (header: game time and steps since the last snapshot, then "<guest address> <calls>" per function that ran) and reset the counters.
// Creating <prefix>.auto switches to one line per game step in <prefix>_steps.txt instead.
extern uint32_t* g_fn_counts;
extern uint32_t* g_fn_touched;   // indices with a non-zero count since the last reset
extern uint32_t g_fn_touched_n;
extern uint32_t* g_fn_lr;   // the return address of the latest entry of each function (its caller)
void vm_trace(uint32_t address, const ppc::Ctx& c);   // UC_VM_TRACE=<file>: every command handler the script evaluator (80077408) calls, with its arguments
extern uint32_t* g_fn_arg;   // r3 of the latest entry (the object of a method)
inline void fn_count(uint32_t address,uint32_t lr,uint32_t arg) {
    if(!g_fn_counts) return;
    const uint32_t index=(address&0x01FFFFFFu)>>2;
    g_fn_lr[index]=lr; g_fn_arg[index]=arg;
    if(!g_fn_counts[index]++) g_fn_touched[g_fn_touched_n++]=index;
}
}
