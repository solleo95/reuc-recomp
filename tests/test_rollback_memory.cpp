#include "check.h"
#include "ppc_fixture.h"
#include "rollback_memory.h"
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>
int main() {
    Fix f;
    auto tracker=std::make_unique<uc::RollbackMemory>(f.c.mem);
    auto& memory=*tracker;
    const auto initial=memory.capture(true);
    const auto hash_start=std::chrono::steady_clock::now();
    const auto initial_hash=uc::RollbackMemory::hash(*initial);
    const auto hash_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-hash_start).count();
    ppc::wr8(f.c,0x8003F000,0); ppc::wr8(f.c,0x80040000,0);
    const auto identical=memory.capture(true);
    CHECK(uc::RollbackMemory::difference(*initial,*identical).empty());
    CHECK(uc::RollbackMemory::hash(*identical)==initial_hash);
    CHECK(uc::rollback_dirty[0x3F]==0 && uc::rollback_dirty[0x40]==0);
    ppc::wr8(f.c,0x8003F000,0); ppc::wr8(f.c,0x80040000,17);
    const auto mixed=memory.capture(true);
    CHECK(uc::RollbackMemory::difference(*identical,*mixed).find("RAM 00040000")==0);
    CHECK(uc::RollbackMemory::hash(*mixed)!=initial_hash);
    // Uncaptured writes must restore both a reused page and a newly allocated page.
    ppc::wr8(f.c,0x8003F000,99); ppc::wr8(f.c,0x80040000,99);
    memory.restore(*mixed);
    CHECK(ppc::rd8(f.c,0x8003F000)==0 && ppc::rd8(f.c,0x80040000)==17);
    CHECK(uc::RollbackMemory::difference(*mixed,*memory.capture(true)).empty());
    memory.restore(*initial);
    ppc::wr32_stack(f.c,0x80001FFF,0x12345678);
    f.c.r[1]=0x90002FFC; f.c.f[0]=1.25; f.c.ps1[0]=-4.5;
    ppc::op_stfd(f.c,0,1,0);
    ppc::psq_store(f.c,0,0xE0000FFC,0,0,true);
    const auto changed=memory.capture(true);
    CHECK(!uc::RollbackMemory::difference(*initial,*changed).empty());
    const auto changed_hash_start=std::chrono::steady_clock::now();
    CHECK(initial_hash!=uc::RollbackMemory::hash(*changed));
    const auto changed_hash_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-changed_hash_start).count();
    gx::watch_reset(); // Resetting texture caches must not erase rollback writes.
    ppc::wr8(f.c,0xC0001000,42);
    memory.restore(*changed);
    CHECK(ppc::rd8(f.c,0x80001000)==0);
    const auto again=memory.capture(true);
    CHECK(uc::RollbackMemory::difference(*changed,*again).empty());
    memory.restore(*initial);
    const auto restored=memory.capture(true);
    CHECK(uc::RollbackMemory::difference(*initial,*restored).empty());
    // Adjacent pages straddle a shared 64-page block; RAM regions and the final partial block stay exact.
    ppc::wr32_stack(f.c,0x8003FFFE,0x11223344);
    ppc::wr8(f.c,0x817FFFFF,17); ppc::wr8(f.c,0x90000000,33);
    ppc::wr8(f.c,0x93FFFFFF,65); ppc::wr8(f.c,0xE000FFFF,129);
    const auto boundary=memory.capture(true);
    const auto boundary_hash=uc::RollbackMemory::hash(*boundary);
    CHECK(boundary_hash!=initial_hash);
    CHECK(uc::RollbackMemory::difference(*initial,*boundary).find("RAM 0003FFFE") == 0);
    ppc::wr32_stack(f.c,0x8007FFFE,0x55667788);
    ppc::wr8(f.c,0xE000FFFF,7);
    const auto later=memory.capture(true);
    uint64_t first_hash=0,second_hash=0;
    std::thread first_reader([&] { first_hash=uc::RollbackMemory::hash(*later); });
    std::thread second_reader([&] { second_hash=uc::RollbackMemory::hash(*later); });
    first_reader.join(); second_reader.join();
    CHECK(first_hash==second_hash && first_hash!=boundary_hash);
    CHECK(!uc::RollbackMemory::difference(*boundary,*later).empty());
    CHECK(uc::RollbackMemory::hash(*boundary)==boundary_hash);
    CHECK(uc::RollbackMemory::hash(*initial)==initial_hash);
    memory.restore(*boundary);
    CHECK(ppc::rd32(f.c,0x8003FFFE)==0x11223344 && ppc::rd32(f.c,0x8007FFFE)==0);
    CHECK(ppc::rd8(f.c,0x817FFFFF)==17 && ppc::rd8(f.c,0x90000000)==33);
    CHECK(ppc::rd8(f.c,0x93FFFFFF)==65 && ppc::rd8(f.c,0xE000FFFF)==129);
    // Uncaptured writes in shared blocks must be restored even when the snapshot references match.
    ppc::wr8(f.c,0x800C0004,42); ppc::wr8(f.c,0xD0000004,43);
    memory.restore(*boundary);
    CHECK(ppc::rd8(f.c,0x800C0004)==0 && ppc::rd8(f.c,0x90000004)==0);
    CHECK(uc::RollbackMemory::difference(*boundary,*memory.capture(true)).empty());
    memory.restore(*later);
    CHECK(ppc::rd32(f.c,0x8007FFFE)==0x55667788 && ppc::rd8(f.c,0xE000FFFF)==7);
    memory.restore(*initial);
    f.c.mem[0x1234]=1;
    bool rejected=false;
    try { memory.capture(true); } catch(const std::logic_error&) { rejected=true; }
    CHECK(rejected);
    uc::rollback_note_write(0x1234);
    memory.restore(*initial);
    const auto capture_start=std::chrono::steady_clock::now();
    for(int frame=0;frame<120;++frame) memory.capture();
    const auto capture_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-capture_start).count();
    const auto dirty_start=std::chrono::steady_clock::now();
    for(int frame=0;frame<120;++frame) {
        for(uint32_t page=0;page<64;++page)ppc::wr8(f.c,0x80000000+page*4096,uint8_t(frame));
        memory.capture();
    }
    const auto dirty_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-dirty_start).count();
    std::printf("rollback memory: first full hash %.3f ms, changed hash %.3f ms, 120 unchanged captures %.3f ms, 120 x 64 dirty pages %.3f ms\n",hash_ms,changed_hash_ms,capture_ms,dirty_ms);
    tracker.reset();
    // Returned snapshots retain their pages after the second tracker leaves scope.
    auto retained=[&] {
        uc::RollbackMemory separate(f.c.mem);
        auto first=separate.capture();
        ppc::wr8(f.c,0x80100000,71);
        auto second=separate.capture();
        return std::pair{std::move(first),std::move(second)};
    }();
    const auto retained_hash=uc::RollbackMemory::hash(*retained.first);
    CHECK(uc::RollbackMemory::hash(*retained.second)!=retained_hash);
    std::weak_ptr<const uc::RollbackMemory::Snapshot> released=retained.second;
    uint64_t read_hash=0;
    std::thread destroy([snapshot=std::move(retained.second)]() mutable { snapshot.reset(); });
    std::thread read([snapshot=retained.first,&read_hash] { read_hash=uc::RollbackMemory::hash(*snapshot); });
    destroy.join(); read.join();
    CHECK(released.expired() && read_hash==retained_hash);
    retained.first.reset(); // The last snapshot releases its retained pages.
}
