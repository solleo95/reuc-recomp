#pragma once
#include "gx_packets.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace uc {
// Screen-space bounding box of every draw in a frame packet (aim coordinates: x right, y down, [-1,1] = the 640x480 EFB).
struct TestDrawStat {
    uint32_t pass = 0, callsite = 0, vertices = 0;   // pass: index of the packet since the request; one entry per draw
    uint32_t tex_w = 0, tex_h = 0; uint64_t tex_id = 0;   // texture 0 dimensions and identity (pointer hash, stable within a run)
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
std::vector<TestDrawStat> summarize_draws(const gx::FramePacket&);
std::array<float,16> scene_projection(const gx::FramePacket&);   // first perspective projection, row-major 4x4; zeros if none
std::string projection_json(const std::array<float,16>&);   // "[16 numbers]", or "" for the all-zero (absent) matrix
std::string test_draws_json(const std::vector<TestDrawStat>&);
void test_draws_publish(std::vector<TestDrawStat>);   // main thread, per rendered presented packet
std::vector<TestDrawStat> test_draws_latest();        // any thread
}
