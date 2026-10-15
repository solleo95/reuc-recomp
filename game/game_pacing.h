#pragma once
#include "ppc_rt.h"
namespace uc {
struct GamePacingStats { uint64_t calls,retraces,timestep_ns,threshold_two,adjusted,wait_ns; };   // wait_ns: time the guest idled in the frame wait, i.e. headroom
void game_pacing_init(bool unlock, uint32_t game_hz = 0);   // game_hz > 0: experimental timer-driven step instead of the retrace wait
GamePacingStats game_pacing_stats();
}
void hle_GameFrameWait(ppc::Ctx& c);
