#include "check.h"
#include "rt_mesh_cache.h"
#include <algorithm>
int main() {
    using uc::RtMeshCachePolicy;
    using Action = RtMeshCachePolicy::Action;
    // A miss builds, the committed entry hits afterwards; the opaque flag is part of the key
    {
        RtMeshCachePolicy cache(1000,4);
        cache.begin_frame(1);
        const auto key=RtMeshCachePolicy::make_key(7,true);
        CHECK(cache.request(key)==Action::build);
        CHECK(cache.commit(key,100).empty() && cache.contains(key) && cache.bytes()==100 && cache.resident()==1);
        CHECK(cache.request(key)==Action::hit);                                   // the same frame: a second draw of the mesh reuses the BLAS built a moment ago
        CHECK(RtMeshCachePolicy::make_key(7,false)!=key && RtMeshCachePolicy::make_key(8,true)!=key);
        CHECK(cache.request(RtMeshCachePolicy::make_key(7,false))==Action::build);   // the cut-out variant of the same mesh is its own BLAS
        CHECK(cache.hits()==1 && cache.builds_requested()==2);
    }
    // At most `builds_per_frame` builds per frame; the rest wait for the next frame (their draws do not occlude meanwhile)
    {
        RtMeshCachePolicy cache(1000,2);
        cache.begin_frame(1);
        CHECK(cache.request(1)==Action::build && cache.request(3)==Action::build && cache.request(5)==Action::defer);
        cache.commit(1,10); cache.commit(3,10);
        CHECK(cache.request(5)==Action::defer);                                    // still over the cap this frame
        cache.begin_frame(2);
        CHECK(cache.request(5)==Action::build && cache.request(1)==Action::hit);
    }
    // The byte budget evicts the least recently used entry that this frame does not use
    {
        RtMeshCachePolicy cache(100,8);
        cache.begin_frame(1); cache.request(10); CHECK(cache.commit(10,60).empty());
        cache.begin_frame(2); cache.request(20); const auto evicted=cache.commit(20,60);
        CHECK(evicted.size()==1 && evicted[0]==10 && !cache.contains(10) && cache.contains(20) && cache.bytes()==60 && cache.evictions()==1);
        // an entry used in the current frame is never evicted, even over budget
        cache.begin_frame(3); CHECK(cache.request(20)==Action::hit); cache.request(30);
        CHECK(cache.commit(30,60).empty() && cache.contains(20) && cache.contains(30) && cache.bytes()==120);
        // ... and goes first when the next frame needs room and does not use it
        cache.begin_frame(4); CHECK(cache.request(30)==Action::hit); cache.request(40);
        const auto next=cache.commit(40,30);
        CHECK(next.size()==1 && next[0]==20 && cache.bytes()==90);
    }
    // The entry count is capped too (every BLAS is its own device allocation): the least recently used leaves, never one used this frame
    {
        RtMeshCachePolicy cache(1000000,8,3);
        for(uint64_t frame=1;frame<=3;++frame) { cache.begin_frame(frame); cache.request(frame*10); cache.commit(frame*10,10); }
        CHECK(cache.resident()==3);
        cache.begin_frame(4); cache.request(40);
        const auto evicted=cache.commit(40,10);
        CHECK(evicted.size()==1 && evicted[0]==10 && cache.resident()==3 && cache.bytes()==30);
        cache.begin_frame(5);
        for(const uint64_t key:{20,30,40}) CHECK(cache.request(key)==Action::hit);
        cache.request(50);
        CHECK(cache.commit(50,10).empty() && cache.resident()==4);   // all used this frame: over the cap until the next frame
    }
    // A freed mesh drops both of its variants
    {
        RtMeshCachePolicy cache(1000,8);
        cache.begin_frame(1);
        for(const auto key:{RtMeshCachePolicy::make_key(5,true),RtMeshCachePolicy::make_key(5,false),RtMeshCachePolicy::make_key(6,true)}) { cache.request(key); cache.commit(key,10); }
        auto dropped=cache.invalidate(5);
        std::sort(dropped.begin(),dropped.end());
        CHECK(dropped.size()==2 && dropped[0]==RtMeshCachePolicy::make_key(5,false) && dropped[1]==RtMeshCachePolicy::make_key(5,true));
        CHECK(cache.resident()==1 && cache.bytes()==10 && cache.contains(RtMeshCachePolicy::make_key(6,true)));
        CHECK(cache.invalidate(99).empty());
    }
    return 0;
}
