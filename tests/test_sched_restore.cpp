#include "check.h"
#include "clock.h"
#include "dispatch.h"
#include "sched.h"
#include <stdexcept>

namespace {
uc::Sched* scheduler;
constexpr uint32_t resume_pc=0x80001000, wait_key=0x1234;
void loop(ppc::Ctx& c) {
    for (;;) {
        ++c.r[7];
        c.lr=resume_pc;
        uc::PendingHle pending(*scheduler,c);
        scheduler->block_on(wait_key);
    }
}
}
int main() {
    const uc::FnEntry table[]={{resume_pc,loop}};
    uc::dispatch_init(table,1,resume_pc,resume_pc+4);
    uc::set_deterministic_clock(true);
    uc::Sched s(nullptr); scheduler=&s; s.set_deterministic(true);
    auto* main=s.create(0,16,[&](ppc::Ctx& c) {
        c.native_depth=2;
        auto* worker=s.create(1,2,loop,&c);
        CHECK(worker->ctx.native_depth==0);
        c.native_depth=0;
        s.resume(worker);
        CHECK(worker->ctx.r[7]==1);
        const auto snapshot=s.checkpoint();
        const auto original_hash=uc::Sched::checkpoint_hash(*snapshot);
        c.ps1[13]=1.25; CHECK(uc::Sched::checkpoint_hash(*s.checkpoint())!=original_hash); c.ps1[13]=0;
        c.gqr[2]=3; CHECK(uc::Sched::checkpoint_hash(*s.checkpoint())!=original_hash); c.gqr[2]=0;
        c.spr[500]=7; CHECK(uc::Sched::checkpoint_hash(*s.checkpoint())!=original_hash); c.spr[500]=0;
        ++c.poll_count; CHECK(uc::Sched::checkpoint_hash(*s.checkpoint())!=original_hash); --c.poll_count;
        CHECK(uc::Sched::checkpoint_hash(*s.checkpoint())==original_hash);
        for(int trial=0;trial<4;++trial) {
            s.wake_all(wait_key);
            CHECK(worker->ctx.r[7]==2);
            s.wake_all(wait_key);
            CHECK(worker->ctx.r[7]==3);
            c.r[8]=123;
            s.restore(*snapshot);
            CHECK(worker->ctx.r[7]==1 && c.r[8]==0);
            CHECK(worker->state==uc::Thread::State::Waiting);
            CHECK(uc::Sched::checkpoint_hash(*s.checkpoint())==original_hash);
        }
        s.wake_all(wait_key);
        CHECK(worker->ctx.r[7]==2);
    });
    s.run(main);
    uc::set_deterministic_clock(false);
}
