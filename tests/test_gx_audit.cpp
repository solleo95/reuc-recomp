#include "check.h"
#include "gx_audit.h"
#include <algorithm>
#include <thread>

int main() {
    ppc::Ctx c{}; c.r[3] = 0x98; c.r[4] = 1; c.r[5] = 4; c.f[1] = 1.5;
    std::vector<std::byte> resource(5000,std::byte{0x61});
    uc::gx_audit_recorded_resource(0x80002000,resource);
    uc::gx_audit_enter(0x80284FD0, c);
    CHECK(uc::gx_audit_snapshot().empty());
    uc::gx_audit_enable();
    uc::gx_audit_enter(0x12345678, c);
    CHECK(uc::gx_audit_snapshot().empty());
    uc::gx_audit_enter(0x80284FD0, c);
    c.r[3] = 0x90;
    uc::gx_audit_enter(0x80284FD0, c);
    auto first = uc::gx_audit_snapshot();
    CHECK(first.size() == 1 && first[0].calls == 2);
    CHECK(first[0].first_gpr[0] == 0x98 && first[0].first_gpr[2] == 4 && first[0].first_fpr[0] == 1.5);
    std::thread producer([&] { for (int i = 0; i < 1000; ++i) uc::gx_audit_enter(0x80366D3C, c); });
    for (int i = 0; i < 1000; ++i) { auto snapshot = uc::gx_audit_snapshot(); CHECK(snapshot.size() <= 2); }
    producer.join();
    auto final = uc::gx_audit_snapshot();
    CHECK(final.size() == 2 && final[1].calls == 1000 && first.size() == 1);
    c.r[3] = 0x80000100; c.r[4] = 64; c.lr = 0x8003F740;
    uc::gx_audit_fifo_write(4,0xFFFFFFFF); // outside a recording
    uc::gx_audit_enter(0x80288528,c);
    uc::gx_audit_fifo_write(1,0x123461);
    uc::gx_audit_fifo_write(4,0x410004A0);
    uc::gx_audit_fifo_write(2,0x1234ABCD);
    uc::gx_audit_enter(0x802885D8,c);
    uc::gx_audit_fifo_write(4,0xFFFFFFFF);
    auto recording = uc::gx_audit_snapshot()[1];
    CHECK(recording.address == 0x80288528 && recording.recording_bytes == 7);
    CHECK(recording.first_caller == 0x8003F73C && recording.first_recording_size == 7);
    CHECK((recording.first_recording_prefix[0] == 0x61 && recording.first_recording_prefix[1] == 0x41 && recording.first_recording_prefix[5] == 0xAB && recording.first_recording_prefix[6] == 0xCD));
    uc::gx_audit_enter(0x80288528,c);
    uc::gx_audit_fifo_write(1,0x90);
    uc::gx_audit_enter(0x802885D8,c);
    auto repeated = uc::gx_audit_snapshot()[1];
    CHECK(repeated.recording_bytes == 8 && repeated.first_recording_size == 7);
    uc::gx_audit_recorded_resource(0x80002000,resource);
    resource[0]=std::byte{0x90};
    uc::gx_audit_recorded_resource(0x80003000,resource);
    auto captured = uc::gx_audit_snapshot()[1];
    CHECK(captured.resources.size()==2 && captured.resources[0].address==0x80002000 && captured.resources[0].size==5000);
    CHECK(captured.resources[0].bytes.size()==4096 && captured.resources[0].bytes[0]==std::byte{0x61});
    uc::gx_audit_recorded_resource(0x80002000,resource); // one owned sample per address
    for (uint32_t address : {0x80004000u,0x80005000u,0x80006000u}) uc::gx_audit_recorded_resource(address,resource);
    CHECK(uc::gx_audit_snapshot()[1].resources.size()==4);
    for (uint32_t address : {0x8003D1ACu,0x8003D250u,0x8003D2F4u,0x8003D380u,0x8003A864u,0x8003A900u}) {
        uc::gx_audit_enter(address,c);
        auto snapshot=uc::gx_audit_snapshot();
        CHECK(std::any_of(snapshot.begin(),snapshot.end(),[address](const auto& entry){return entry.address==address && entry.calls==1;}));
    }
    c.lr=0x8003D208; // verified indirect update call at8003D204
    uc::gx_audit_enter(0x80123456,c);
    auto callbacks=uc::gx_audit_snapshot();
    auto callback=std::find_if(callbacks.begin(),callbacks.end(),[](const auto& entry){return entry.address==0x80123456;});
    CHECK(callback!=callbacks.end() && callback->first_caller==0x8003D204 && callback->calls==1);
    c.lr=0x80000004;
    uc::gx_audit_enter(0x80123456,c);
    callbacks=uc::gx_audit_snapshot();
    callback=std::find_if(callbacks.begin(),callbacks.end(),[](const auto& entry){return entry.address==0x80123456;});
    CHECK(callback!=callbacks.end() && callback->calls==1);
    for (uint32_t link : {0x8003D2ACu,0x8003D348u,0x8003D408u,0x8003D430u,0x8003D4A8u,0x80131740u,0x801317D4u,0x801317E8u,0x80131850u,0x8013188Cu,0x801318A4u,0x801318C4u,0x801318DCu,0x80131908u}) {
        c.lr=link;
        uc::gx_audit_enter(0x80123456,c);
    }
    callbacks=uc::gx_audit_snapshot();
    callback=std::find_if(callbacks.begin(),callbacks.end(),[](const auto& entry){return entry.address==0x80123456;});
    CHECK(callback!=callbacks.end() && callback->calls==15);
    for (uint32_t link : {0x80115348u,0x8011535Cu,0x80116204u}) {
        c.lr=link;
        uc::gx_audit_enter(0x80123456,c);
    }
    callbacks=uc::gx_audit_snapshot();
    callback=std::find_if(callbacks.begin(),callbacks.end(),[](const auto& entry){return entry.address==0x80123456;});
    CHECK(callback!=callbacks.end() && callback->calls==18);
    return 0;
}
