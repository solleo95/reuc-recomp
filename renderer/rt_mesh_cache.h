#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
namespace uc {
// The policy of the cached object-space BLASes of static meshes (docs/superpowers/specs/2026-10-19-rt-level-shadows-design.md, design 4): which mesh gets a
// BLAS built this frame, which waits, which entries go when the byte budget is exceeded. It knows nothing of Vulkan: RtScene creates and destroys the objects
// this class names. A key is a mesh id and its opaque flag (the cut-out variant of a mesh is its own BLAS).
class RtMeshCachePolicy {
public:
    enum class Action { hit, build, defer };
    RtMeshCachePolicy(uint64_t budget_bytes, uint32_t builds_per_frame) : budget_(budget_bytes), cap_(builds_per_frame) {}
    static uint64_t make_key(uint64_t mesh_id, bool opaque) { return mesh_id*2+(opaque ? 1 : 0); }
    void begin_frame(uint64_t frame) { frame_=frame; builds_=0; }
    // A draw of the mesh asks for its BLAS: resident (hit, marked used this frame), to be built now (build, at most `builds_per_frame` per frame; the caller then
    // calls commit), or later (defer: the draw does not occlude this frame).
    Action request(uint64_t key) {
        if (const auto found=entries_.find(key); found!=entries_.end()) { found->second.last_use=frame_; ++hits_; return Action::hit; }
        if (builds_>=cap_) return Action::defer;
        ++builds_; ++builds_requested_;
        return Action::build;
    }
    // The BLAS of `key` was built and is `bytes` big. Returns the keys evicted to stay within the budget: the least recently used that this frame does not
    // use (an entry used this frame stays even when the cache goes over budget).
    std::vector<uint64_t> commit(uint64_t key, uint64_t bytes) {
        if (const auto old=entries_.find(key); old!=entries_.end()) bytes_-=old->second.bytes;
        entries_[key]={bytes,frame_}; bytes_+=bytes;
        std::vector<uint64_t> evicted;
        while (bytes_>budget_) {
            uint64_t victim=0, oldest=UINT64_MAX; bool found=false;
            for (const auto& [candidate,entry] : entries_) if (candidate!=key && entry.last_use<frame_ && entry.last_use<oldest) { victim=candidate; oldest=entry.last_use; found=true; }
            if (!found) break;
            bytes_-=entries_[victim].bytes; entries_.erase(victim); evicted.push_back(victim); ++evictions_;
        }
        return evicted;
    }
    // The mesh was freed: both of its variants go.
    std::vector<uint64_t> invalidate(uint64_t mesh_id) {
        std::vector<uint64_t> dropped;
        for (const bool opaque : {false,true}) {
            const uint64_t key=make_key(mesh_id,opaque);
            if (const auto found=entries_.find(key); found!=entries_.end()) { bytes_-=found->second.bytes; entries_.erase(found); dropped.push_back(key); }
        }
        return dropped;
    }
    bool contains(uint64_t key) const { return entries_.contains(key); }
    uint64_t bytes() const { return bytes_; }
    size_t resident() const { return entries_.size(); }
    uint64_t hits() const { return hits_; }
    uint64_t builds_requested() const { return builds_requested_; }
    uint64_t evictions() const { return evictions_; }
private:
    struct Entry { uint64_t bytes; uint64_t last_use; };
    std::unordered_map<uint64_t,Entry> entries_;
    uint64_t budget_, bytes_=0, frame_=0, hits_=0, builds_requested_=0, evictions_=0;
    uint32_t cap_, builds_=0;
};
}
