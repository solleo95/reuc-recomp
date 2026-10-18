#include "check.h"
#include "motion_tracker.h"
#include <cmath>
namespace {
gx::Projection perspective() { return {{{1.5f,0,0,0},{0,1.5f,0,0},{0,0,-1,-1},{0,0,-1,0}}}; }   // GX layout: w = -z
gx::Transforms at(float x, float y, float z, gx::Projection projection=perspective()) {
    gx::Transforms t; t.projection=projection; t.current=0;
    t.position_rows[0]={1,0,0,x}; t.position_rows[1]={0,1,0,y}; t.position_rows[2]={0,0,1,z};
    return t;
}
gx::DrawPacket draw(uint32_t callsite, const std::shared_ptr<const gx::MeshBuffers>& mesh) {
    gx::DrawPacket d; d.callsite=callsite; d.mesh=mesh; return d;
}
bool near(const gx::Projection& a, const gx::Projection& b) {
    for(size_t r=0;r<4;++r) for(size_t c=0;c<4;++c) if(std::abs(a[r][c]-b[r][c])>1e-4f) return false;
    return true;
}
}
int main() {
    auto mesh=std::make_shared<gx::MeshBuffers>(); mesh->vertices.resize(300);
    auto other=std::make_shared<gx::MeshBuffers>(); other->vertices.resize(30);
    {   // pairing by callsite, mesh and order
        gx::MotionTracker tracker;
        tracker.begin_frame();
        auto a=tracker.key(draw(1,mesh)); tracker.record(a,at(0,0,-10),true);
        auto b=tracker.key(draw(1,mesh)); tracker.record(b,at(5,0,-10),true);
        CHECK(a.occurrence==0 && b.occurrence==1 && a.vertices==300);
        tracker.end_frame();
        tracker.begin_frame();
        auto a2=tracker.key(draw(1,mesh)); auto b2=tracker.key(draw(1,mesh)); auto c2=tracker.key(draw(2,other));
        CHECK(a2==a && b2==b);
        const auto* pa=tracker.previous(a2); const auto* pb=tracker.previous(b2);
        CHECK(pa && pb && pa->palette.position[0][3]==0 && pb->palette.position[0][3]==5);
        CHECK(near(pa->projection,perspective()));
        CHECK(!tracker.previous(c2));   // new this frame: unpaired
        tracker.record(a2,at(1,0,-10),true); tracker.record(b2,at(6,0,-10),true); tracker.record(c2,at(0,0,-5),false);
        tracker.end_frame();
        CHECK(!tracker.camera_cut());
        tracker.begin_frame();
        CHECK(tracker.previous(tracker.key(draw(1,mesh)))->palette.position[0][3]==1);
    }
    {   // a packet rolled back (renderer retry after an acquire timeout) leaves no trace: same occurrences, no records
        gx::MotionTracker tracker;
        tracker.begin_frame(); tracker.record(tracker.key(draw(1,mesh)),at(0,0,-10),true); tracker.end_frame();
        tracker.begin_frame();
        const auto first=tracker.key(draw(1,mesh)); tracker.record(first,at(1,0,-10),true);
        const auto mark=tracker.checkpoint();
        auto second=tracker.key(draw(1,mesh)); tracker.record(second,at(2,0,-10),true);
        CHECK(second.occurrence==1);
        tracker.rollback(mark);
        auto again=tracker.key(draw(1,mesh)); CHECK(again.occurrence==1);   // counted once, not twice
        tracker.rollback(mark);
        tracker.end_frame();
        tracker.begin_frame();
        auto next=tracker.key(draw(1,mesh));
        CHECK(tracker.previous(next) && tracker.previous(next)->palette.position[0][3]==1);   // the kept record
        auto rolled=tracker.key(draw(1,mesh)); CHECK(!tracker.previous(rolled));            // the rolled-back one is gone
    }
    {   // non-mesh draws pair by callsite and vertex count
        gx::MotionTracker tracker;
        gx::DrawPacket quad; quad.callsite=7; quad.vertices.resize(4);
        tracker.begin_frame(); tracker.record(tracker.key(quad),at(0,0,-3),false); tracker.end_frame();
        tracker.begin_frame();
        CHECK(tracker.previous(tracker.key(quad)));
        gx::DrawPacket longer=quad; longer.vertices.resize(8);
        CHECK(!tracker.previous(tracker.key(longer)));
    }
    {   // identical frames: no camera motion; a moved camera is reprojected; a jump is a cut
        gx::MotionTracker tracker;
        for(int frame=0;frame<2;++frame) { tracker.begin_frame(); tracker.record(tracker.key(draw(1,mesh)),at(0,0,-10),true); tracker.record(tracker.key(draw(2,other)),at(1,1,-8),true); tracker.end_frame(); }
        const gx::Projection identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
        CHECK(near(tracker.clip_to_prev_clip(),identity) && !tracker.camera_cut());
        // camera moves +0.5 in x: every view-space position shifts by -0.5
        tracker.begin_frame(); tracker.record(tracker.key(draw(1,mesh)),at(-0.5f,0,-10),true); tracker.record(tracker.key(draw(2,other)),at(0.5f,1,-8),true); tracker.end_frame();
        const auto m=tracker.clip_to_prev_clip();
        const std::array<float,4> now{-0.5f*1.5f,0,10-1,10};   // clip of the largest mesh's origin this frame (view -0.5,0,-10)
        std::array<float,4> back{};
        for(size_t r=0;r<4;++r) for(size_t c=0;c<4;++c) back[r]+=m[r][c]*now[c];
        CHECK(std::abs(back[0]/back[3]-0.0f)<1e-4f && std::abs(back[3]-10)<1e-3f);   // previous clip: origin at x 0
        CHECK(!tracker.camera_cut());
        tracker.begin_frame(); tracker.record(tracker.key(draw(1,mesh)),at(40,0,-10),true); tracker.record(tracker.key(draw(2,other)),at(41,1,-8),true); tracker.end_frame();
        CHECK(tracker.camera_cut());
        tracker.begin_frame(); tracker.record(tracker.key(draw(1,mesh)),at(40,0,-10,{{{3,0,0,0},{0,3,0,0},{0,0,-1,-1},{0,0,-1,0}}}),true); tracker.end_frame();
        CHECK(tracker.camera_cut());   // field of view doubled
    }
    return 0;
}
