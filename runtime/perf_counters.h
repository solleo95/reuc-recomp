#pragma once
#include <cstdint>
// Hardware counters of the guest threads (Linux; elsewhere everything is a no-op). UC_PERF_COUNTERS=1 turns them on: every guest thread opens user-space
// instruction and cycle counters for itself when it starts, and perf_totals() adds them up over all guest threads. The counters see only the work of the
// guest threads (translated code, HLE called from it), not the renderer, the driver or the kernel: the number of instructions for the same deterministic
// run does not depend on code layout, clock speed or other load, which wall time and CPU time do. docs/testing/performance-measurement.md is the protocol.
namespace uc {
struct PerfTotals { bool enabled = false; uint64_t instructions = 0, cycles = 0; };
bool perf_counters_enabled();
void perf_register_thread();   // the calling thread; once per thread
PerfTotals perf_totals();
}
