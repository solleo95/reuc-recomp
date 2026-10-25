#include "check.h"
#include "netplay_game.h"
#include <bit>
#include <limits>
int main() {
    CHECK(uc::net_startup_rtt({100,100,100,100,100,100,100,400,900})==100);
    CHECK(uc::net_startup_rtt({900,400,100,100,100,100,100,100,100})==100);
    CHECK(uc::net_startup_rtt({100,100,100,100,200,200,200,200,900})==200);
    CHECK(uc::net_startup_rtt({0,0,0,0,0,0,0,0,0})==0);
    CHECK(uc::net_repair_end(0,3)==0);
    CHECK(uc::net_repair_end(0,4)==1);
    CHECK(uc::net_repair_end(0,30)==8);
    CHECK(uc::net_repair_end(8,30)==16);
    CHECK(uc::net_repair_end(24,30)==27);
    CHECK(uc::net_repair_end(28,30)==28);
    CHECK(uc::net_repair_end(UINT64_MAX-12,UINT64_MAX)==UINT64_MAX-4);
    uc::NetInputClock recovery(8);
    CHECK(recovery.sample(0,0)==8);
    CHECK(recovery.sample(0,50000000,true)==14);
    uc::NativeInput recovery_input;
    recovery_input.focus(true); recovery_input.context(uc::InputContext::gameplay);
    recovery_input.host_button({uc::PhysicalKind::mouse,3},true);
    // Forward progress drains already-sent slots instead of retaining extra latency forever.
    for(uint64_t cursor=1;cursor<7;++cursor) {
        if(cursor==4)recovery_input.host_button({uc::PhysicalKind::mouse,3},false);
        CHECK(!recovery.sample(cursor,50000000+cursor*8333333));
        CHECK(recovery.next()==15);
    }
    CHECK(recovery.sample(7,108333331)==15);
    const auto recovered_input=recovery_input.capture_frame();
    const auto fire=uint64_t(1)<<unsigned(uc::Action::fire);
    CHECK(recovered_input.actions.count==2);
    CHECK((recovered_input.actions.pressed&fire) && (recovered_input.actions.released&fire));
    CHECK(!(recovered_input.actions.held&fire));
    CHECK(recovery.sample(8,116666664)==16);
    uc::NetInputClock sampling(8);
    CHECK(sampling.sample(0,0)==8);
    CHECK(sampling.sample(1,8333333)==9);
    CHECK(!sampling.sample(0,10000000));
    // A rewind cannot suspend input for the peer, or overwrite an already sent frame.
    CHECK(sampling.sample(0,16666666,true)==10);
    CHECK(!sampling.sample(1,20000000));
    CHECK(sampling.sample(2,24999999,true)==11);
    CHECK(!sampling.sample(2,25000000));
    CHECK(!sampling.sample(3,26000000));
    CHECK(sampling.sample(4,27000000)==12);
    CHECK(sampling.next()==13);
    bool skipped=false;
    try { sampling.sample(6,28000000); } catch(const std::logic_error&) { skipped=true; }
    CHECK(skipped);
    CHECK(sampling.sample(0,60000000,true)==15); // Slots13/14 keep old held state; new edges belong only to15.
    CHECK(sampling.next()==16 && !sampling.sample(0,60000000));
    CHECK(sampling.sample(0,61000000,true)==16);
    CHECK(uc::net_input_delay(0)==2);
    CHECK(uc::net_input_delay(40000000)==5);
    CHECK(uc::net_input_delay(100000000)==8);
    CHECK(uc::net_input_delay(200000000)==14);
    CHECK(uc::net_input_delay(240000000)==17);
    CHECK(uc::net_input_delay(400000000)==26);
    CHECK(uc::net_input_delay(433333333)==28);
    for(const auto rtt:{uint64_t(433333334),UINT64_MAX}) {
        bool rejected=false;
        try {uc::net_input_delay(rtt);}catch(const std::invalid_argument&) {rejected=true;}
        CHECK(rejected);
    }
    uc::NativeInput source; source.focus(true); source.context(uc::InputContext::qte);
    uc::NetQteState window;
    window.update(10,1,3,true,&source);
    const auto first_epoch=window.epoch;
    source.host_motion(32,0,1000000000);
    window.update(10,1,6,true,&source);
    source.host_motion(-32,0,1030000000);
    CHECK(source.consume_actions().count==0); // A new target cannot inherit half a gesture.
    CHECK(window.epoch==first_epoch+1);
    window.update(10,1,6,true,&source); CHECK(window.epoch==first_epoch+1);
    uc::NetFrame stale; stale.qte_epoch=first_epoch;
    stale.input.actions.events[0]={uc::Action::qte_shake,true,0,0,true};
    stale.input.actions.events[1]={uc::Action::fire,true,0,0,false};
    stale.input.actions.count=2;
    stale.input.actions.held=stale.input.actions.pressed=uint64_t(1)<<unsigned(uc::Action::qte_shake);
    auto current=stale; current.qte_epoch=window.epoch;
    window.filter(current); CHECK(current.input.actions.count==2);
    window.filter(stale);
    CHECK(stale.input.actions.count==1 && stale.input.actions.events[0].action==uc::Action::fire);
    CHECK(stale.input.actions.held==0 && stale.input.actions.pressed==0);
    window.update(10,1,6,false,&source); window.update(10,1,6,true,&source);
    window.filter(current); CHECK(current.input.actions.count==1); // Reopened target has a new epoch.
    const auto reopened_epoch=window.epoch;
    window.update(11,1,6,true,&source); window.update(11,2,6,true,&source);
    CHECK(window.epoch==reopened_epoch+2);
    CHECK(uc::net_work_estimate({8000000,8000000,40000000},8000000)==8000000);
    CHECK(uc::net_work_estimate({8000000,17000000,17000000},8000000)==17000000);
    CHECK(uc::net_work_estimate({8000000,8000000,8000000},17000000)>16000000);
    uc::NetFrame original;original.number=17;original.checksum=123;original.work_ns=8300000;original.qte_epoch=9;
    original.input.sample={-.3f,.7f,.1f,-0.f,0x400,0x400,0,true};
    original.input.host=false;
    original.input.generation=original.input.actions.generation=4;
    original.input.actions.events[0]={uc::Action::reload,true,0,0,false};original.input.actions.count=1;
    const auto wire=uc::encode_net_frame(original);
    CHECK(wire.size()==119 && wire[0]=='U' && wire[1]=='C' && wire[2]=='F' && wire[3]==1);
    CHECK(wire[4]==17 && wire[5]==0); // uint64 LE frame number follows the four-byte marker.
    auto decoded=uc::decode_net_frame(wire);
    CHECK(uc::same_net_input(original,decoded));
    auto unrelated_epoch=original; ++unrelated_epoch.qte_epoch;
    CHECK(uc::same_net_input(original,unrelated_epoch));
    for(bool active:{false,true})for(auto epoch:{original.qte_epoch,unrelated_epoch.qte_epoch}) {
        uc::NetQteState filter; filter.active=active; filter.epoch=epoch;
        auto left=original,right=unrelated_epoch;
        filter.filter(left); filter.filter(right);
        left.qte_epoch=right.qte_epoch=0;
        CHECK(uc::encode_net_frame(left)==uc::encode_net_frame(right));
    }
    auto shake=original; shake.input.actions.events[0].action=uc::Action::qte_shake;
    auto stale_shake=shake; ++stale_shake.qte_epoch;
    CHECK(!uc::same_net_input(shake,stale_shake));
    auto held_shake=original;
    held_shake.input.actions.held=uint64_t(1)<<unsigned(uc::Action::qte_shake);
    auto stale_held_shake=held_shake; ++stale_held_shake.qte_epoch;
    CHECK(!uc::same_net_input(held_shake,stale_held_shake));
    CHECK(std::bit_cast<uint32_t>(decoded.input.sample.dy)==0x80000000u);
    CHECK(uc::encode_net_frame(decoded)==wire);
    auto predicted=uc::predict_net_frame(original,18);
    CHECK(predicted.number==18 && predicted.input.sample.x==original.input.sample.x);
    CHECK(predicted.input.sample.held==original.input.sample.held && predicted.input.sample.pressed==0);
    CHECK(predicted.input.sample.dx==0 && predicted.input.actions.count==0);
    CHECK(!uc::same_net_input(original,predicted));
    auto stable=uc::predict_net_frame(predicted,19);
    CHECK(uc::same_net_input(predicted,stable));
    stable.input.sample.x+=.01f; CHECK(!uc::same_net_input(predicted,stable));
    CHECK(decoded.number==17 && decoded.checksum==123 && decoded.input.sample.x==-.3f);
    CHECK(decoded.work_ns==8300000 && decoded.qte_epoch==9);
    CHECK(decoded.input.actions.count==1 && decoded.input.actions.events[0].action==uc::Action::reload);
    auto rejects=[](std::span<const uint8_t> data) { try {uc::decode_net_frame(data);return false;}catch(const std::exception&) {return true;} };
    CHECK(rejects(std::span<const uint8_t>{}));
    for(size_t size=0;size<wire.size();++size)CHECK(rejects(std::span(wire).first(size)));
    auto extra=wire; extra.push_back(0); CHECK(rejects(extra));
    const auto mutation=[&](size_t offset,uint64_t value,unsigned width) {
        auto changed=wire;
        for(unsigned i=0;i<width;++i)changed[offset+i]=uint8_t(value>>(i*8));
        CHECK(rejects(changed));
    };
    mutation(0,'X',1); mutation(3,2,1); // Magic and version.
    mutation(20,1000000001,8); // Work estimate bound.
    mutation(36,0x7FC00000,4); mutation(40,std::bit_cast<uint32_t>(1.01f),4);
    mutation(44,std::bit_cast<uint32_t>(2.01f),4);
    mutation(52,1,4); mutation(56,1,4); mutation(60,1,4); // Guest button masks.
    mutation(64,2,1); mutation(65,2,1); // Sample-valid and host flags.
    mutation(66,5,8); mutation(98,5,8); // Generation agreement.
    mutation(74,UINT64_MAX,8); mutation(82,UINT64_MAX,8); mutation(90,UINT64_MAX,8);
    mutation(106,257,2); mutation(106,0,2); // Count overflow and extra event bytes.
    mutation(108,unsigned(uc::Action::count),1); mutation(109,2,1); mutation(118,2,1);
    mutation(110,0x7FC00000,4); mutation(114,std::bit_cast<uint32_t>(-1.01f),4);
    auto maximum=original;
    maximum.number=maximum.checksum=maximum.qte_epoch=UINT64_MAX; maximum.work_ns=1000000000;
    maximum.input.generation=maximum.input.actions.generation=UINT64_MAX;
    auto& actions=maximum.input.actions;
    actions.held=actions.pressed=actions.released=(uint64_t(1)<<unsigned(uc::Action::count))-1;
    actions.count=actions.events.size();
    for(size_t i=0;i<actions.count;++i)actions.events[i]={uc::Action(i%unsigned(uc::Action::count)),bool(i%2),-1,1,bool(i%3)};
    const auto maximum_wire=uc::encode_net_frame(maximum);
    CHECK(maximum_wire.size()==108+256*11);
    const auto maximum_decoded=uc::decode_net_frame(maximum_wire);
    CHECK(uc::same_net_input(maximum,maximum_decoded));
    CHECK(maximum_decoded.number==UINT64_MAX && maximum_decoded.checksum==UINT64_MAX && maximum_decoded.qte_epoch==UINT64_MAX);
    CHECK(uc::encode_net_frame(maximum_decoded)==maximum_wire);
    for(size_t size=108;size<maximum_wire.size();++size)CHECK(rejects(std::span(maximum_wire).first(size)));
    auto bad=original;bad.input.sample.x=std::numeric_limits<float>::quiet_NaN();
    bool failed=false;try {uc::decode_net_frame(uc::encode_net_frame(bad));}catch(const std::exception&){failed=true;}CHECK(failed);
    bad=original;bad.input.actions.events[0].action=uc::Action(255);
    failed=false;try {uc::decode_net_frame(uc::encode_net_frame(bad));}catch(const std::exception&){failed=true;}CHECK(failed);
    bad=original;bad.work_ns=1000000001;
    failed=false;try {uc::decode_net_frame(uc::encode_net_frame(bad));}catch(const std::exception&){failed=true;}CHECK(failed);
}
