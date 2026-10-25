#include "check.h"
#include "audio_startup.h"
#include "clock.h"
#include "dispatch.h"
#include "hle/os.h"
#include "ppc_fixture.h"
namespace {
constexpr uint32_t manager=0x8045D4F0,entry=0x80049D64;
void load(ppc::Ctx& c) {
    CHECK(c.lr==0);
    uc::audio_wait_ready(c,10);
    c.r[7]=42;
}
}
int main() {
    Fix f;
    const uc::FnEntry table[]={{entry,load}};
    uc::dispatch_init(table,1,entry,entry+4);
    uc::set_deterministic_clock(true);
    uc::Sched s(f.c.mem); s.set_deterministic(true);
    uc::os_hle_init(s,f.c.mem,0x817F0000);
    ppc::wr8(f.c,manager+0x16C,1);
    auto* main=s.create(0,16,[&](ppc::Ctx& c) {
        auto* worker=s.create(1,2,[](ppc::Ctx& c) { c.r[3]=manager;c.lr=0;load(c); });
        s.resume(worker);
        const auto scheduler=s.checkpoint();
        const auto audio=uc::audio_startup_checkpoint();
        for(unsigned pass=0;pass<3;++pass) {
            ppc::wr32(c,manager+0x1710,manager+0x1574);ppc::wr32(c,manager+0x1FA4,2);
            s.join(worker);CHECK(worker->ctx.r[7]==42);
            s.restore(*scheduler);uc::audio_startup_restore(*audio);
            ppc::wr32(c,manager+0x1710,0);ppc::wr32(c,manager+0x1FA4,0);
            CHECK(worker->ctx.r[7]==0);
        }
        // Deadline also rewinds: a never-ready player eventually releases the loader.
        s.join(worker);CHECK(worker->ctx.r[7]==42);
    });
    s.run(main);uc::set_deterministic_clock(false);
}
