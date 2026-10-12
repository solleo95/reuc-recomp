#include "check.h"
#include "dispatch.h"
#include "hle/os.h"
#include "ppc_fixture.h"
#include "sched.h"
#include <string>
#include <vector>
using namespace ppc;

void hle_DSPAddTask(Ctx&);

static std::vector<std::string> g_log;
static void init_cb(Ctx& c) { g_log.push_back("init(" + std::to_string(c.r[3]) + ")"); uc::os_sched().wake_all(0x6100); }

int main() {
    Fix f; Ctx& c = f.c;
    uc::Sched s(c.mem);
    static const uc::FnEntry table[] = {{0x80100000, init_cb}};
    uc::dispatch_init(table, 1, 0x80100000, 0x80100004);
    uc::os_hle_init(s, c.mem, 0x817F0000);
    s.irq_ctx().r[1] = 0x817EFF00;
    uc::Thread* m = s.create(0, 16, [&](Ctx& t) {
        const uint32_t task = 0x80362000;
        wr32(t, task + 0x28, 0x80100000);                // init_cb
        t.r[3] = task; hle_DSPAddTask(t);
        CHECK(t.r[3] == task);                           // returns the task
        CHECK(rd32(t, task + 0x00) == 0 && rd32(t, task + 0x08) == 1);   // state INIT, flags: running
        s.block_on(0x6100);                              // the "DSP" reports its boot in interrupt context
        CHECK(g_log.size() == 1 && g_log[0] == "init(" + std::to_string(task) + ")");
        wr32(t, task + 0x28, 0);                         // no callback: nothing to run
        t.r[3] = task; hle_DSPAddTask(t); CHECK(t.r[3] == task);
    });
    s.run(m);
    return 0;
}
