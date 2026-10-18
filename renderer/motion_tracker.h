#pragma once
#include "gx_transforms.h"
#include "gx_vertices.h"
#include <compare>
#include <map>
#include <vector>

namespace gx {
// Identity of a draw across guest frames for motion vectors: callsite, mesh (stable while the guest keeps it) or vertex count, and the
// order among draws with the same (callsite, mesh, vertices) in the frame.
struct DrawKey {
    uint32_t callsite=0;
    const void* mesh=nullptr;
    uint32_t vertices=0, occurrence=0;
    auto operator<=>(const DrawKey&) const=default;
};
struct PreviousDraw { PaletteBlock palette; Projection projection; };
// Hands the vertex shader last frame's matrices of the same draw. Camera motion estimate and cut detection come from the paired draws.
class MotionTracker {
public:
    static constexpr float kCameraCutNdc=0.5f;          // median screen displacement of paired draws (NDC units) that counts as a cut
    static constexpr float kProjectionCutRelative=0.2f; // relative change of the projection scale (field of view) that counts as a cut
    void begin_frame();
    DrawKey key(const DrawPacket& draw);
    const PreviousDraw* previous(const DrawKey& key) const;
    void record(const DrawKey& key, const Transforms& transforms, bool z_write);
    void end_frame();
    // Undo the key()/record() calls made since checkpoint() (a packet the renderer retries later).
    size_t checkpoint() const { return journal_.size(); }
    void rollback(size_t mark);
    bool camera_cut() const { return cut_; }                       // of the frame finished by the last end_frame()
    const Projection& clip_to_prev_clip() const { return clip_to_prev_; }
private:
    struct Entry { PreviousDraw draw; std::array<std::array<float,4>,3> rows; bool z_write; bool mesh; };
    std::map<DrawKey,Entry> previous_, current_;
    std::map<std::tuple<uint32_t,const void*,uint32_t>,uint32_t> occurrences_;
    std::vector<DrawKey> journal_;   // keys handed out this frame, in order
    bool cut_=false;
    Projection clip_to_prev_{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
};
}
