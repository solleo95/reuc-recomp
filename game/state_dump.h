#pragma once
#include "ppc_rt.h"
namespace uc {
// Diagnostic for frame-rate dependence: UC_STATE_DUMP=<prefix>|<seconds>,<seconds>,... Once <prefix>.go exists it adds up the game time of every step and writes the whole
// guest RAM (MEM1 then MEM2) to <prefix>_<seconds>.bin at the first step that reaches each listed time. Run the same scripted scene at two step rates and compare the
// snapshots word by word (tools/spikes/state_diff.py): what moved the same distance at the same game time follows the real step, the rest counts calls.
void state_dump_step(const ppc::Ctx&, float timestep);
}
