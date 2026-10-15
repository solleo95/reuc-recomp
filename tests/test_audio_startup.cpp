#include "check.h"
#include "audio_startup.h"
#include "clock.h"
#include "hle/os.h"
#include "ppc_fixture.h"
int main() {
    Fix f;uc::Sched scheduler(f.c.mem);
    uc::os_hle_init(scheduler,f.c.mem,0x817F0000);
    constexpr uint32_t manager=0x8045D4F0;
    ppc::wr8(f.c,manager+0x16C,1);
    bool entered=false,finished=false;
    auto* main=scheduler.create(0,16,[&](ppc::Ctx& c) {
        // Player never initializes: the wait gives up and lets the original loader run (old behaviour) instead of aborting the game.
        bool gave_up=false;
        auto* stuck=scheduler.create(0x80001000,10,[&](ppc::Ctx& worker) {
            worker.r[3]=manager;worker.r[4]=12;
            uc::audio_wait_ready(worker,5);
            gave_up=true;
        });
        scheduler.resume(stuck);
        scheduler.join(stuck);
        CHECK(gave_up);
        // Archive pointer already attached on arrival is not readiness: the worker still waits for state 2.
        ppc::wr32(c,manager+0x1710,manager+0x1574);
        auto* loader=scheduler.create(0x80001000,10,[&](ppc::Ctx& worker) {
            worker.r[3]=manager;worker.r[4]=12;worker.lr=0x80102D94;
            entered=true;uc::audio_wait_ready(worker);
            CHECK(ppc::rd32(worker,manager+0x1FA4)==2);
            CHECK(worker.r[3]==manager && worker.r[4]==12 && worker.lr==0x80102D94);
            finished=true;
        });
        scheduler.resume(loader);
        CHECK(entered && !finished);
        scheduler.sleep_ticks(uc::ms_to_ticks(3));
        CHECK(!finished); // Attaching the archive precedes completion of player initialization.
        ppc::wr32(c,manager+0x1FA4,2);
        scheduler.join(loader);
        CHECK(finished);
    });
    scheduler.run(main);scheduler.shutdown();scheduler.join_hosts();
    // Ready, disabled and unrelated calls never need a running scheduler.
    f.c.r[3]=manager;uc::audio_wait_ready(f.c);
    ppc::wr32(f.c,manager+0x1FA4,0);ppc::wr8(f.c,manager+0x16C,0);
    uc::audio_wait_ready(f.c);
    f.c.r[3]=0;uc::audio_wait_ready(f.c);
}
