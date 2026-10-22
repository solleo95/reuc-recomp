#pragma once
#include "gx_transforms.h"
#include "gx_vertices.h"
#include <compare>
#include <memory>
#include <memory_resource>
#include <unordered_map>
#include <vector>

namespace gx {
// Identity of a draw across guest frames for motion vectors: callsite, mesh (stable while the guest keeps it) or vertex count, and the
// instance among draws with the same (callsite, mesh, vertices): the previous frame's occurrence it pairs with (nearest predicted position),
// or a fresh number past last frame's for an instance without a partner.
struct DrawKey {
    uint32_t callsite=0;
    const void* mesh=nullptr;
    uint32_t vertices=0, occurrence=0;
    auto operator<=>(const DrawKey&) const=default;
};
struct PreviousDraw { std::shared_ptr<const Transforms> transforms; };
// Hands the vertex shader last frame's matrices of the same draw. Camera motion estimate and cut detection come from the paired draws.
class MotionTracker {
public:
    explicit MotionTracker(std::pmr::memory_resource* upstream=std::pmr::get_default_resource());
    static constexpr float kCameraCutNdc=0.5f;          // median screen displacement of paired draws (NDC units) that counts as a cut
    static constexpr float kProjectionCutRelative=0.2f; // relative change of the projection scale (field of view) that counts as a cut
    void begin_frame();
    DrawKey key(const DrawPacket& draw);
    const PreviousDraw* previous(const DrawKey& key) const;
    void record(const DrawKey& key, std::shared_ptr<const Transforms> transforms, bool z_write);
    void end_frame();
    // Undo the key()/record() calls made since checkpoint() (a packet the renderer retries later).
    size_t checkpoint() const { return journal_.size(); }
    void rollback(size_t mark);
    bool camera_cut() const { return cut_; }                       // of the frame finished by the last end_frame()
    const Projection& clip_to_prev_clip() const { return clip_to_prev_; }
    // The camera's motion in view space: the previous frame's view-space position of a point that stands still in the world, from its view-space position in the
    // finished frame (rows of a 3x4 matrix: the rotation and the translation; the identity after a cut or without a paired level draw).
    const std::array<std::array<float,4>,3>& view_to_prev_view() const { return view_to_prev_; }
private:
    struct Entry { PreviousDraw draw; std::array<std::array<float,4>,3> rows; bool z_write; bool mesh; };
    // Last frame's instances sorted by key, one span per (callsite, mesh, vertices) identity; a draw claims the nearest unclaimed one.
    struct Instance { DrawKey key; std::array<float,3> origin; bool claimed; };
    struct Span { uint32_t first, count, next, cursor; };   // next: past the highest occurrence; cursor: first unclaimed (relative)
    uint32_t claim(Span& span, const std::array<float,3>& origin);
    struct DrawKeyHash {
        size_t operator()(const DrawKey& key) const {
            size_t hash=std::hash<const void*>{}(key.mesh);
            for(uint32_t value:{key.callsite,key.vertices,key.occurrence})
                hash^=std::hash<uint32_t>{}(value)+0x9e3779b9u+(hash<<6)+(hash>>2);
            return hash;
        }
    };
    std::pmr::unsynchronized_pool_resource pool_;
    std::pmr::unordered_map<DrawKey,Entry,DrawKeyHash> previous_, current_;
    std::pmr::unordered_map<DrawKey,uint32_t,DrawKeyHash> occurrences_;   // instances without a partner handed out this frame
    std::pmr::vector<Instance> instances_;
    std::pmr::unordered_map<DrawKey,Span,DrawKeyHash> spans_;
    std::vector<DrawKey> journal_;   // keys handed out this frame, in order
    std::vector<float> moves_;
    bool cut_=false;
    Projection clip_to_prev_{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};
    std::array<std::array<float,4>,3> view_to_prev_{{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
};
}
