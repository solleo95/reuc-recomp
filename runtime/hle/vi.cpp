#include "vi.h"
#include "guest_call.h"
#include "hle/os.h"
#include <atomic>

using namespace uc;

namespace {
constexpr uint32_t kViKey = 0xF4000001u;
constexpr uint32_t kRetraceCount = uint32_t(-25948), kPostCb = uint32_t(-25992), kPreCb = uint32_t(-25996);   // offsets from r13 (retrace handler at 8026D1C0, setters at 8026D8C4 / 8026D908)
struct Vi {
    std::atomic<uint64_t> retraces{0};
    std::atomic<uint32_t> cur_fb{0}, next_fb{0}, tv{0};
    std::atomic<bool> black{false}; // VISetBlack; starts released so a missed release can never blank the port for good
    uint64_t period = kViPeriodNtsc;
    std::function<void()> hook;
} g;

void arm(uint64_t at) {
    os_sched().post_at(at, [at] {
        Sched& s = os_sched();
        uint64_t n = ++g.retraces;
        g.cur_fb = g.next_fb.load();                            // a frame buffer set during the last frame goes live now
        arm(next_periodic(at, g.period, now_ticks()));          // re-arm first: callbacks may take long
        // The SDK keeps the retrace state in small-data globals; VIGetRetraceCount and VISet*RetraceCallback run translated on them.
        ppc::Ctx& ic = s.irq_ctx();
        uint32_t base = ic.r[13];
        ppc::wr32(ic, base + kRetraceCount, ppc::rd32(ic, base + kRetraceCount) + 1);
        if (uint32_t pre = ppc::rd32(ic, base + kPreCb)) call_guest(ic, pre, {uint32_t(n)});
        if (g.hook) g.hook();
        if (uint32_t post = ppc::rd32(ic, base + kPostCb)) call_guest(ic, post, {uint32_t(n)});
        s.wake_all(kViKey);
    });
}
}  // namespace

namespace uc {
void vi_hle_init(Sched&, uint64_t period_ticks, std::function<void()> on_retrace) {
    g.period = period_ticks;
    g.hook = std::move(on_retrace);
    arm(now_ticks() + period_ticks);
}
ViStats vi_stats() { return {g.retraces.load(), g.cur_fb.load(), g.next_fb.load(), g.tv.load(), g.black.load()}; }
}  // namespace uc

void hle_VIInit(ppc::Ctx&) {}
void hle_VIConfigure(ppc::Ctx& c) {   // r3 = GXRenderModeObj*; viTVmode (first word) >> 2: 0 NTSC, 1 PAL, 2 MPAL, 3 DEBUG, 4 DEBUG_PAL, 5 EURGB60
    if (!c.r[3]) return;
    g.tv = ppc::rd32(c, c.r[3]) >> 2;
    g.period = (g.tv == 1 || g.tv == 4) ? kViPeriodPal : kViPeriodNtsc;   // PAL and DEBUG_PAL run at 50 Hz
}
void hle_VIFlush(ppc::Ctx&) {}
void hle_VISetNextFrameBuffer(ppc::Ctx& c) { g.next_fb = c.r[3]; }
void hle_VISetBlack(ppc::Ctx& c) { g.black = c.r[3] != 0; }
void hle_VIGetTvFormat(ppc::Ctx& c) { c.r[3] = g.tv == 1 || g.tv == 4 ? 1 : g.tv == 2 ? 2 : g.tv == 5 ? 5 : 0; }   // VI_NTSC 0, VI_PAL 1, VI_MPAL 2, VI_EURGB60 5
void hle_VIWaitForRetrace(ppc::Ctx&) { os_sched().block_on(kViKey); }
