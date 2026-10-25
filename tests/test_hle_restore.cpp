#include "check.h"
#include "clock.h"
#include "dispatch.h"
#include "hle/os.h"
#include "hle/gx.h"
#include "native_input.h"
#include "ppc_fixture.h"
#include "vi.h"
#include <thread>

void hle_OSInitMessageQueue(ppc::Ctx&);
void hle_OSReceiveMessage(ppc::Ctx&);
void hle_OSSendMessage(ppc::Ctx&);
void hle_OSSetAlarm(ppc::Ctx&);
void hle_OSCancelAlarm(ppc::Ctx&);
void hle_VISetNextFrameBuffer(ppc::Ctx&);
void hle_VISetBlack(ppc::Ctx&);
void hle_GXSetCopyClear(ppc::Ctx&);
void hle_GXCopyDisp(ppc::Ctx&);
namespace {
constexpr uint32_t queue=0x80300000,output=0x80301000,resume=0x80100000,alarm_pc=0x80100010;
int alarms=0;
void receive(ppc::Ctx& c) {
    c.r[3]=queue; c.r[4]=output; c.r[5]=1; c.lr=resume;
    hle_OSReceiveMessage(c);
}
void consumed(ppc::Ctx& c) {
    for(;;) {
        CHECK(c.r[3]==1);
        ++c.r[20]; c.r[21]=ppc::rd32(c,output);
        receive(c);
    }
}
void alarm(ppc::Ctx&) { ++alarms; }
}
int main() {
    Fix f;
    const uc::FnEntry table[]={{resume,consumed},{alarm_pc,alarm}};
    uc::dispatch_init(table,2,resume,alarm_pc+4);
    uc::set_deterministic_clock(true);
    uc::Sched s(f.c.mem); s.set_deterministic(true);
    uc::os_hle_init(s,f.c.mem,0x817F0000);
    auto* main=s.create(0,16,[&](ppc::Ctx& c) {
        c.r[3]=queue; c.r[5]=1; hle_OSInitMessageQueue(c);
        auto* worker=s.create(1,2,[](ppc::Ctx& c) { receive(c); consumed(c); });
        s.resume(worker);
        CHECK(worker->state==uc::Thread::State::Waiting);
        const auto sync=uc::os_sync_checkpoint();
        const auto checkpoint=s.checkpoint();
        for(int pass=0;pass<5;++pass) {
            for(uint32_t message:{37u,41u}) {
                c.r[3]=queue; c.r[4]=message; c.r[5]=0; hle_OSSendMessage(c);
                CHECK(c.r[3]==1 && worker->ctx.r[21]==message);
            }
            CHECK(worker->ctx.r[20]==2);
            s.restore(*checkpoint); uc::os_sync_restore(*sync);
            CHECK(worker->ctx.r[20]==0);
        }
        c.r[3]=0x80302000; c.r[5]=0; c.r[6]=uc::ms_to_ticks(100); c.r[7]=alarm_pc;
        hle_OSSetAlarm(c);
        const auto alarm_state=uc::os_alarm_checkpoint();
        const auto before_alarm=s.checkpoint();
        const auto tick=uc::det_clock_checkpoint();
        hle_OSCancelAlarm(c);
        uc::det_clock_advance_to(tick+uc::ms_to_ticks(200)); s.safe_point();
        CHECK(alarms==0);
        s.restore(*before_alarm); uc::os_alarm_restore(*alarm_state);
        CHECK(uc::det_clock_checkpoint()==tick);
        uc::det_clock_advance_to(tick+uc::ms_to_ticks(200)); s.safe_point();
        CHECK(alarms==1);

        const auto before_io=s.checkpoint();
        const auto stale=s.async_begin();
        bool rejected=false;
        try { s.checkpoint(); } catch(const std::logic_error&) { rejected=true; }
        CHECK(rejected);
        s.restore(*before_io);
        int delivered=0;
        std::thread completion([&] { s.post_external([&] { ++delivered; },uc::Sched::EventKind::Dvd,stale); });
        completion.join();
        s.safe_point(); CHECK(delivered==0);
        s.checkpoint();

        c.r[3]=0x80400000; hle_VISetNextFrameBuffer(c);
        const auto vi=uc::vi_checkpoint();
        c.r[3]=1; hle_VISetBlack(c); c.r[3]=0; hle_VISetNextFrameBuffer(c);
        uc::vi_restore(vi);
        CHECK(!uc::vi_stats().black && uc::vi_stats().next_fb==0x80400000);
        gx::Packets packets;
        uc::gx_hle_init(s,c.mem,&packets);
        c.r[2]=0x803F0000; ppc::wr32(c,c.r[2]-8968,0x80001000);
        ppc::wr32(c,output,0x10203040);
        c.r[3]=output; c.r[4]=0x112233; hle_GXSetCopyClear(c);
        const auto graphics=uc::gx_checkpoint();
        ppc::wr32(c,output,0x50607080);
        c.r[3]=output; c.r[4]=0x445566; hle_GXSetCopyClear(c);
        uc::gx_restore(*graphics);
        c.r[3]=0x80400000; c.r[4]=1; hle_GXCopyDisp(c);
        auto packet=packets.try_take();
        CHECK(packet && packet->clear_color[0]==0x10 && packet->clear_color[3]==0x40 && packet->clear_depth==0x112233);
        CHECK(packets.complete(packet->serial));
        uc::NativeInput pad;
        uc::InputFrame input;
        input.sample.pressed=uc::kButtonA; input.sample.held=uc::kButtonA;
        pad.install_frame(input);
        const auto pad_state=pad.checkpoint();
        CHECK(pad.consume().pressed==uc::kButtonA);
        pad.install_frame(uc::InputFrame{});
        CHECK(pad.consume().pressed==0);
        pad.restore(*pad_state);
        CHECK(pad.consume().pressed==uc::kButtonA);
        s.post_at(UINT64_MAX,[] {});
        rejected=false;
        try { s.checkpoint(); } catch(const std::logic_error&) { rejected=true; }
        CHECK(rejected);
    });
    s.run(main);
    uc::set_deterministic_clock(false);
}
