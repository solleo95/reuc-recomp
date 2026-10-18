#pragma once
#include "gx_packets.h"
#include <array>
#include <iosfwd>
#include <optional>
#include <vector>

namespace gx {
// What the scene/HUD boundary rule looks at, per draw and per packet. UC_SCENE_DUMP (game/packet_dump.h) writes these as text.
struct DrawSummary {
    uint32_t callsite=0;
    bool perspective=false, z_test=false, z_write=false;
    std::optional<std::array<float,6>> viewport;
    uint32_t vertices=0;
    uint8_t blend_mode=0, blend_source=1, blend_destination=0;
};
struct PacketSummary {
    std::vector<DrawSummary> draws;
    std::optional<std::array<uint32_t,4>> copy_source;   // EFB copy to a texture, 640x480 coordinates
    bool present=false;
};
DrawSummary summarize(const DrawPacket& draw);
PacketSummary summarize(const FramePacket& packet);
// Text: "P <present> <x,y,w,h|->" then one "D <callsite> <persp> <ztest> <zwrite> <vp 6 floats|-> <vertices> <mode:src:dst>" per draw.
void write_packet(std::ostream& out, const PacketSummary& packet);
std::vector<PacketSummary> read_packets(std::istream& in);

// Where the 3D scene of a guest frame ends (docs/spikes/S2-motion-vectors.md, "Scene boundary"): the first packet after a perspective
// draw that has no draws and copies the whole last perspective viewport, i.e. the game's copy of the finished scene that feeds its post
// chain (bloom) and precedes the HUD. Fed every packet in order; the state of a frame ends at its present.
class SceneBoundary {
public:
    void reset();
    // Draw index before which the scene ends in this packet (always 0 for the rule above), or none.
    std::optional<size_t> on_packet(const PacketSummary& packet);
    bool frame_had_scene() const { return had_scene_; }   // the last finished frame (or the current one before its present) had perspective draws
    const std::optional<std::array<float,6>>& scene_viewport() const { return viewport_; }   // last perspective viewport of the scene
private:
    bool perspective_=false, cut_=false, had_scene_=false;
    std::optional<std::array<float,6>> viewport_;
};
}
