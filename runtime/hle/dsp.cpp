// DSP task manager replaced at call level (tech-debt TD-011): a submitted task is accepted and its init callback reports the "boot",
// but no DSP microcode runs, so no task ever produces output.
#include "guest_call.h"
#include "hle/os.h"
#include "ppc_rt.h"
using namespace ppc;

namespace { constexpr uint32_t kState = 0x00, kFlags = 0x08, kInitCb = 0x28; }   // DSPTaskInfo field offsets (from __DSP_boot_task / DSPAddTask)

void hle_DSPAddTask(Ctx& c) {
    uint32_t task = c.r[3], cb = rd32(c, task + kInitCb);
    wr32(c, task + kState, 0);
    wr32(c, task + kFlags, 1);
    if (cb) {
        uc::Sched& s = uc::os_sched();
        s.post_external([&s, cb, task] { uc::call_guest(s.irq_ctx(), cb, {task}); },uc::Sched::EventKind::Dsp);
    }
}
