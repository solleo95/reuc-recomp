#include "check.h"
#include "late_aim.h"
#include "gx_textures.h"
#include <memory>
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
    const std::array<float,2> aim{-0.6f, 0.4f};   // NDC (-0.75, -0.667): the screen position of that aim
    const float nx = -0.75f, ny = -0.667f;
    CHECK(gx::is_reticle_draw(quad(0x80393B0C, nx, ny, 154), aim));
    CHECK(gx::is_reticle_draw(quad(0x8006D174, nx + 0.05f, ny, 100), aim));     // recoil moved it a little
    CHECK(!gx::is_reticle_draw(quad(0x80393B0C, 0.5f, 0.5f, 154), aim));        // the same ring far from the pointer is something else
    CHECK(!gx::is_reticle_draw(quad(0x80393B0C, nx, ny, 64), aim));             // not a reticle texture
    CHECK(!gx::is_reticle_draw(quad(0x80123456, nx, ny, 154), aim));            // not a reticle callsite
    CHECK(!gx::is_reticle_draw(quad(0x80393B0C, nx, ny, 154, true), aim));      // still decoding: decided on a later frame
    return 0;
}
