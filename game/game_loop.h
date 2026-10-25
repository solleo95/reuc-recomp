#pragma once
#include "ppc_rt.h"
#include <functional>

namespace uc {
// Configure before guest startup or after all guest threads have stopped.
// Runs once at the outer iteration boundary, before8003F670 changes registers.
void game_loop_set_boundary(std::function<void(ppc::Ctx&)> boundary);
}
void hle_GameLoop(ppc::Ctx& c);
