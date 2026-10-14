#include "check.h"
#include "dff.h"
#include "memory.h"
#include <algorithm>
#include <functional>

using namespace fifo_replay;
static void put(std::vector<std::byte>& b, size_t p, uint64_t v, size_t n=4) {
    for(size_t i=0;i<n;++i) b.at(p+i)=std::byte(v>>(i*8));
}
static std::vector<std::byte> fixture() {
    std::vector<std::byte> b(128+64+48+7);
    put(b,0,0x0D01F1F0); put(b,4,6); put(b,8,1);
    put(b,60,128,8); put(b,68,1); put(b,72,1);
    put(b,88,24*1024*1024); put(b,92,64*1024*1024);
    put(b,128,240,8); put(b,136,3);
    put(b,148,192,8); put(b,156,2);
    for(size_t p : {192u,216u}) {
        put(b,p,0); put(b,p+4,0x1000); put(b,p+8,243,8);
        put(b,p+16,4); put(b,p+20,4,1);
    }
    put(b,243,0x12345678);
    return b;
}
static void rejects(const std::function<void(std::vector<std::byte>&)>& change) {
    auto b=fixture(); change(b);
    bool failed=false;
    try { (void)parse_dff(std::move(b)); } catch(const std::runtime_error&) { failed=true; }
    CHECK(failed);
}
template<class F> static void fails(F f) {
    bool failed=false;
    try { f(); } catch(const std::runtime_error&) { failed=true; }
    CHECK(failed);
}
static void memory_test() {
    auto b=fixture(); put(b,216,2); put(b,220,0x1002);
    auto d=parse_dff(std::move(b));
    d.frames.push_back(d.frames[0]);
    Memory m(d);
    fails([&]{m.read(0x1000,1);});
    fails([&]{m.advance(1,0);});
    m.advance(0,0);
    CHECK(m.read(0x80001000,4)[2]==std::byte{0x34});
    fails([&]{m.read(0x1000,6);});
    m.advance(0,2);
    CHECK(m.read(0xC0001000,6)[2]==std::byte{0x78});
    CHECK(m.read(0x1000,6)[5]==std::byte{0x12});
    fails([&]{m.advance(0,1);});
    fails([&]{m.read(0x20000000,1);});
    m.advance(0,3); m.advance(1,0);
    CHECK(m.read(0x1000,6)[2]==std::byte{0x34});
    fails([&]{m.advance(0,3);});
    auto bad=parse_dff(fixture()); bad.frames[0].updates[0].address=0x0FFFFFFF;
    Memory invalid(bad); fails([&]{invalid.advance(0,0);});
    auto other=parse_dff(fixture());
    other.frames[0].updates[0].address=0x10000000;
    other.frames[0].updates[1].address=0x10000001;
    other.frames[0].updates[1].type=8;
    Memory mem2(other); mem2.advance(0,0);
    CHECK(mem2.read(0x90000000,5)[1]==std::byte{0x78});
    CHECK(mem2.read(0xD0000000,5)[4]==std::byte{0x12});
    fails([&]{mem2.read(0,1);});
    auto empty=parse_dff(fixture()); empty.frames[0].fifo.size=0;
    empty.frames.push_back(empty.frames[0]);
    Memory zero(empty); fails([&]{zero.advance(1,0);});
    zero.advance(0,0); zero.advance(1,0);
}
int main(int argc,char** argv) try {
    if(argc==2) {
        auto d=load_dff(argv[1]);
        CHECK(!d.frames.empty());
        Memory m(d);
        for(size_t f=0;f<d.frames.size();++f) m.advance(f,d.frames[f].fifo.size);
        std::printf("DFF: %zu frames, %zu bytes\n",d.frames.size(),d.bytes.size());
        return 0;
    }
    auto d=parse_dff(fixture());
    memory_test();
    auto legacy=fixture(); put(legacy,4,5); put(legacy,96,0xFFFFFFFF);
    CHECK(parse_dff(std::move(legacy)).game_id[0]==0);
    CHECK(d.wii && d.frames.size()==1);
    CHECK(d.frames[0].updates.size()==2);
    CHECK(d.frames[0].updates[0].address==0x1000);
    CHECK(d.data(d.frames[0].fifo).size()==3);
    auto moved=std::move(d);
    CHECK(moved.data(moved.frames[0].updates[0].data)[0]==std::byte{0x78});
    rejects([](auto& b){b.resize(127);});
    rejects([](auto& b){put(b,0,0);});
    rejects([](auto& b){put(b,4,7);});
    rejects([](auto& b){put(b,8,7);});
    rejects([](auto& b){put(b,60,UINT64_MAX,8);});
    rejects([](auto& b){put(b,68,UINT32_MAX);});
    rejects([](auto& b){put(b,136,100);});
    rejects([](auto& b){put(b,192,4);}); // update beyond FIFO
    rejects([](auto& b){put(b,192,1);}); // updates out of order
    rejects([](auto& b){put(b,200,UINT64_MAX,8);});
    rejects([](auto& b){put(b,212,99,1);});
    rejects([](auto& b){put(b,20,257);}); // oversized register bank
    rejects([](auto& b){put(b,88,UINT32_MAX);});
    auto banks=fixture();
    put(banks,12,243,8); put(banks,20,1);
    put(banks,76,243,8); put(banks,84,4);
    auto bank_data=parse_dff(std::move(banks));
    CHECK(bank_data.bp[0]==0x12345678 && bank_data.bp[1]==0);
    CHECK(bank_data.data(bank_data.tmem)[0]==std::byte{0x78});
} catch(const std::exception& e) {
    std::fprintf(stderr,"%s\n",e.what());
    return 1;
}
