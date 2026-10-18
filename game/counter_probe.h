#pragma once
#include "ppc_rt.h"
namespace uc {
// Diagnostic for frame-rate dependence: UC_COUNTER_PROBE=<file>|<steps>. Once <file>.go exists it snapshots guest RAM at every game step, finds the words that
// advance by the same amount in (nearly) every step (free-running counters, per-call increments) and writes them to <file> after <steps> steps.
// Run it at two game rates and compare the amount per game second: a counter whose per-step amount stays equal while the step rate changes runs too fast.
void counter_probe_step(const ppc::Ctx&, float timestep);
}
