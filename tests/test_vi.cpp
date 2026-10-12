#include "check.h"
#include "clock.h"
#include "dispatch.h"
#include "hle/os.h"
#include "ppc_fixture.h"
#include "sched.h"
#include "vi.h"
using namespace ppc;

void hle_VIWaitForRetrace(Ctx&); void hle_VISetNextFrameBuffer(Ctx&);
void hle_VIConfigure(Ctx&); void hle_VIGetTvFormat(Ctx&);

static int g_cb_calls = 0, g_pre_calls = 0; static uint32_t g_cb_arg = 0;
static void post_cb(Ctx& c) { ++g_cb_calls; g_cb_arg = c.r[3]; }
static void pre_cb(Ctx&) { ++g_pre_calls; }

int main() {
    Fix f; Ctx& cc = f.c;
    uc::Sched s(cc.mem);
    static const uc::FnEntry table[] = {{0x80100000, post_cb}, {0x80100010, pre_cb}};
    uc::dispatch_init(table, 2, 0x80100000, 0x80100014);
    uc::os_hle_init(s, cc.mem, 0x817F0000);
    s.irq_ctx().r[1] = 0x817EFF00;
    int hook_calls = 0;
    uc::vi_hle_init(s, uc::ms_to_ticks(10), [&] { ++hook_calls; });
    uc::Thread* m = s.create(0, 16, [&](Ctx& c) {
        // The retrace state lives in the SDK's small-data globals (VIGetRetraceCount / VISet*RetraceCallback run translated):
        // count at r13-25948, post callback at r13-25992, pre callback at r13-25996.
        c.r[13] = 0x80430000;
        wr32(c, c.r[13] - 25992, 0x80100000); wr32(c, c.r[13] - 25996, 0x80100010);
        c.r[3] = 0x80400000; hle_VISetNextFrameBuffer(c);
        CHECK(uc::vi_stats().current_fb == 0);          // a new frame buffer only goes live at the next retrace
        uint64_t a = uc::now_ticks();
        for (int i = 0; i < 5; ++i) hle_VIWaitForRetrace(c);
        uint64_t ms = uc::ticks_to_ms(uc::now_ticks() - a);
        CHECK(ms >= 40 && ms < 400);                    // five retraces at 10 ms
        CHECK(uc::vi_stats().current_fb == 0x80400000);
        CHECK(rd32(c, c.r[13] - 25948) >= 5);           // the guest-visible retrace count advances
        CHECK(hook_calls >= 5 && g_cb_calls >= 5 && g_pre_calls >= 5 && g_cb_arg >= 5);   // callbacks get the retrace count in r3
        // a PAL render mode (viTVmode >> 2 == 1) switches the period to 50 Hz
        wr32(c, 0x80410000, 1u << 2);
        c.r[3] = 0x80410000; hle_VIConfigure(c);
        CHECK(uc::vi_stats().tv_mode == 1);
        c.r[3] = 0; hle_VIGetTvFormat(c); CHECK(c.r[3] == 1);
        wr32(c, 0x80410000, 5u << 2); c.r[3] = 0x80410000; hle_VIConfigure(c);   // EURGB60
        hle_VIGetTvFormat(c); CHECK(c.r[3] == 5);
        wr32(c, 0x80410000, 4u << 2); c.r[3] = 0x80410000; hle_VIConfigure(c);   // DEBUG_PAL counts as PAL
        hle_VIGetTvFormat(c); CHECK(c.r[3] == 1);
    });
    s.run(m);
    return 0;
}
