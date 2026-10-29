#pragma once
#include "gx_packets.h"
#include <functional>

namespace gx {
struct SceneNode {
    std::shared_ptr<const SceneNode> previous;
    std::shared_ptr<const FramePacket> command;
};
struct SceneFrame {
    std::shared_ptr<const SceneNode> state;
    FramePacket presentation;
};
struct SceneSnapshot {
    std::shared_ptr<const SceneNode> state;
    bool scene_frame=false;   // the producer's ultrawide memory (ultrawide.h): the unfinished frame already had a 3D scene
};
// Only the unfinished logical frame belongs to a checkpoint. Temporal GPU effects
// use the last rendered texture version and never make simulation wait for rendering.
class SceneCapture {
public:
    using Snapshot=SceneSnapshot;
    std::shared_ptr<const SceneFrame> record(FramePacket packet);
    Snapshot checkpoint() const { return current_; }
    void restore(const Snapshot& saved) { current_=saved; }
private:
    Snapshot current_;
};
std::vector<std::shared_ptr<const FramePacket>> scene_commands(
    const SceneFrame& frame,const std::function<bool(const std::shared_ptr<const TexturePixels>&,bool)>& resident);
}
