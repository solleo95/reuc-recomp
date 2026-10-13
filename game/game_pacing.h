#pragma once
#include "ppc_rt.h"
namespace uc {
struct GamePacingStats { uint64_t calls,retraces,timestep_ns,threshold_two,adjusted; };
void game_pacing_init(bool unlock);
GamePacingStats game_pacing_stats();
}
void hle_GameFrameWait(ppc::Ctx& c);
