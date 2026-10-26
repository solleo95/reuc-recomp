#include "check.h"
#include "late_aim.h"
#include "gx_textures.h"
#include <memory>
#include <optional>
#include <vector>
// Reticle selection of the late-aim shift: callsite, square texture of a known edge, centred near the pointer the game drew with.
static gx::DrawPacket quad(uint32_t callsite, float cx, float cy, uint32_t texture_edge, bool decoding = false) {
    gx::DrawPacket draw;
    draw.callsite = callsite;
    gx::Transforms t; t.projection = {{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};   // vertices are given directly in NDC
    draw.transforms = t;
    gx::State state; draw.state = state;
    draw.vertices.resize(4);
    const float h = 0.08f;
    draw.vertices[0].position = {cx - h, cy - h, 0}; draw.vertices[1].position = {cx + h, cy - h, 0};
    draw.vertices[2].position = {cx + h, cy + h, 0}; draw.vertices[3].position = {cx - h, cy + h, 0};
    auto texture = std::make_shared<gx::TexturePixels>();
    texture->levels.push_back({texture_edge, texture_edge, {}});
    if (decoding) texture->job = std::make_shared<gx::TextureJob>();
    draw.texture = texture;
    return draw;
}
int main() {
    const std::array<float,2> aim{-0.6f, 0.4f};   // Vulkan NDC (-0.75, 0.667), y down: the screen position of that aim (live: aim y 0.2 drew the ring at NDC y +0.333)
    const float nx = -0.75f, ny = 0.667f;
    using Aims = std::array<std::optional<std::array<float,2>>,2>;
    const auto tinted = [](gx::DrawPacket draw, std::array<uint8_t,4> color) { for (auto& vertex : draw.vertices) vertex.color[0] = color; return draw; };
    const std::array<uint8_t,4> red{255,0,0,255}, blue{0,0,100,255};
    const auto mark = [&](std::vector<gx::DrawPacket>& draws, Aims aims, unsigned local) { return gx::mark_reticle_draws(draws, aims, local); };
    {   // Single-player: one pad; the ring, its white companion and the reload halo are the reticle, a far square or another texture is not.
        std::vector<gx::DrawPacket> draws{tinted(quad(0x80393B0C, nx, ny, 154), red), quad(0x8006D174, nx + 0.05f, ny, 100), quad(0x80393B0C, nx, ny, 178),
                                          quad(0x80393B0C, nx, ny, 64), quad(0x80123456, nx, ny, 154), quad(0x80393B0C, nx, ny, 154, true), tinted(quad(0x80393B0C, 0.9f, -0.9f, 154), red)};
        CHECK(mark(draws, {aim, std::nullopt}, 0) == 3);
        CHECK(draws[0].late_aim && draws[0].reticle && draws[1].late_aim && draws[2].late_aim);
        CHECK(!draws[3].reticle && !draws[4].reticle && !draws[5].reticle && !draws[6].reticle);
        std::vector<gx::DrawPacket> blue_alone{tinted(quad(0x80393B0C, nx, ny, 154), blue)};
        CHECK(mark(blue_alone, {aim, std::nullopt}, 0) == 1 && blue_alone[0].late_aim);   // a lone pad's reticle is the first player's whatever its tint
    }
    {   // Cooperative play: the tint names the owner, however far the eased reticle is from its pointer, and however close the other one is.
        const std::array<float,2> other{0.5f, -0.2f};   // pad 1's pointer
        std::vector<gx::DrawPacket> draws{tinted(quad(0x80393B0C, nx, ny, 120), red), quad(0x80393B0C, nx, ny, 120), tinted(quad(0x80393B0C, nx + 0.05f, ny, 120), blue), quad(0x80393B0C, nx + 0.05f, ny, 120)};
        CHECK(mark(draws, {aim, other}, 0) == 2);   // pad 1's reticle sits 1.1 NDC from its pointer: not accepted; pad 0's two quads are
        CHECK(draws[0].late_aim && draws[1].late_aim && !draws[2].reticle && !draws[3].reticle);
        std::vector<gx::DrawPacket> overlap{tinted(quad(0x80393B0C, nx, ny, 120), red), quad(0x80393B0C, nx, ny, 120), tinted(quad(0x80393B0C, nx + 0.1f, ny + 0.02f, 120), blue), quad(0x80393B0C, nx + 0.1f, ny + 0.02f, 120)};
        const std::array<float,2> near_other{aim[0] + 0.05f, aim[1]};
        CHECK(mark(overlap, {aim, near_other}, 1) == 4);   // the client owns pad 1: only the blue reticle follows its mouse, both are scaled
        CHECK(!overlap[0].late_aim && !overlap[1].late_aim && overlap[2].late_aim && overlap[3].late_aim && overlap[0].reticle && overlap[1].reticle);
        {   // Exact mode: every quad and the wake of the local player move from the tinted ring's centre to the mouse, other players' quads are untouched.
            std::vector<gx::DrawPacket> exact{tinted(quad(0x80393B0C, nx, ny, 120), red), tinted(quad(0x80393B0C, nx + 0.1f, ny + 0.02f, 120), blue), quad(0x8006D174, nx + 0.15f, ny + 0.02f, 100),
                                              tinted(quad(0x8006CED4, 0.f, 0.f, 100), blue), tinted(quad(0x8006CED4, 0.f, 0.f, 100), red)};
            CHECK(gx::mark_reticle_draws(exact, Aims{aim, near_other}, 1, true) == 4);
            const float ax = (nx + 0.1f) / gx::kAimToNdcX, ay = (ny + 0.02f) / gx::kAimToNdcY;
            CHECK(!exact[0].aim_exact && exact[1].aim_exact && exact[2].aim_exact && exact[3].aim_exact && !exact[4].aim_exact);
            CHECK(std::abs(exact[1].aim_ref[0] - ax) < 1e-5f && std::abs(exact[2].aim_ref[1] - ay) < 1e-5f && std::abs(exact[3].aim_ref[0] - ax) < 1e-5f);
        }
        {   // Reticles exactly on top of each other: the tint and the draw order still keep the quads apart.
            std::vector<gx::DrawPacket> stacked{tinted(quad(0x80393B0C, nx, ny, 120), red), quad(0x80393B0C, nx, ny, 120), tinted(quad(0x80393B0C, nx + 0.01f, ny, 120), blue), quad(0x80393B0C, nx + 0.01f, ny, 120)};
            CHECK(mark(stacked, {aim, aim}, 0) == 4);
            CHECK(stacked[0].late_aim && stacked[1].late_aim && !stacked[2].late_aim && !stacked[3].late_aim && stacked[2].reticle && stacked[3].reticle);
        }
        std::vector<gx::DrawPacket> far{tinted(quad(0x80393B0C, nx - 0.45f, ny, 120), red)};   // a fast flick: 0.45 from the pointer it was drawn from
        CHECK(mark(far, {aim, other}, 0) == 1 && far[0].late_aim);
    }
    {   // The wake (callsite 8006CED4, in the player's tint) follows only the local reticle and is not scaled.
        std::vector<gx::DrawPacket> draws{tinted(quad(0x8006CED4, 0.f, 0.f, 100), red), tinted(quad(0x8006CED4, 0.f, 0.f, 100), blue), quad(0x8006CED4, 0.f, 0.f, 100)};
        const std::array<float,2> other{0.5f, -0.2f};
        CHECK(mark(draws, {aim, other}, 1) == 1 && !draws[0].late_aim && draws[1].late_aim && !draws[2].late_aim && !draws[1].reticle);
        std::vector<gx::DrawPacket> single{tinted(quad(0x8006CED4, 0.f, 0.f, 100), blue)};
        CHECK(mark(single, {aim, std::nullopt}, 0) == 1 && single[0].late_aim);
    }
    // Crosshair scale: the viewport shrinks about the quad's centre, which keeps its pixel (pixel = x + (ndc * 0.5 + 0.5) * width).
    float x = 10, y = 20, width = 1280, height = 720;
    gx::scale_viewport_about({nx, ny}, 0.5f, x, y, width, height);
    CHECK(width == 640 && height == 360);
    CHECK(std::abs(x + (nx * .5f + .5f) * width - (10 + (nx * .5f + .5f) * 1280)) < 1e-3f);
    CHECK(std::abs(y + (ny * .5f + .5f) * height - (20 + (ny * .5f + .5f) * 720)) < 1e-3f);
    return 0;
}
