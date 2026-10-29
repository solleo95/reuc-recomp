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
        std::vector<gx::DrawPacket> flick{tinted(quad(0x80393B0C, nx, ny, 154), red), quad(0x8006D174, nx + 0.9f, ny, 100)};   // the dot trails far behind in a fast flick
        CHECK(mark(flick, {aim, std::nullopt}, 0) == 2 && flick[1].late_aim);
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
            CHECK(std::abs(exact[2].aim_ref[0] - (nx + 0.15f) / gx::kAimToNdcX) < 1e-5f);   // the dot lands on the mouse itself, not at its offset from the ring
        }
        {   // A tinted dot drawn before the ring, inside its group: the anchor is still the ring's own centre (it flipped with the group's first member and the reticle shook).
            std::vector<gx::DrawPacket> both{tinted(quad(0x8006D174, nx + 0.02f, ny, 100), red), tinted(quad(0x80393B0C, nx, ny, 154), red)};
            CHECK(gx::mark_reticle_draws(both, Aims{aim, std::nullopt}, 0, true) == 2);
            CHECK(std::abs(both[1].aim_ref[0] - nx / gx::kAimToNdcX) < 1e-5f && std::abs(both[0].aim_ref[0] - (nx + 0.02f) / gx::kAimToNdcX) < 1e-5f);
        }
        {   // The Game Over page: a leftover ring of the mission sits at another place and is drawn first; the pointer's ring is the one nearest the pointer the game read,
            // so it is the anchor (the first ring as the anchor moved the real one twice: the reticle showed twice as far from the centre as the pointer was).
            std::vector<gx::DrawPacket> page{tinted(quad(0x80393B0C, -0.3f, 0.2f, 154), red), tinted(quad(0x80393B0C, nx, ny, 154), red)};
            CHECK(gx::mark_reticle_draws(page, Aims{aim, std::nullopt}, 0, true) == 2);
            CHECK(std::abs(page[1].aim_ref[0] - nx / gx::kAimToNdcX) < 1e-5f && std::abs(page[1].aim_ref[1] - ny / gx::kAimToNdcY) < 1e-5f);
        }
        {   // The first-aid spray icon (bottom left) comes through the ring's callsite with a 100 x 100 texture: it is not part of the reticle, even with the pointer on it
            // (it moved with the reticle). The laser dot has the same edge through its own callsite and stays one.
            std::vector<gx::DrawPacket> hud{tinted(quad(0x80393B0C, nx, ny, 154), red), quad(0x80393B0C, nx + 0.02f, ny, 100), quad(0x8006D174, nx + 0.03f, ny, 100)};
            CHECK(gx::mark_reticle_draws(hud, Aims{aim, std::nullopt}, 0, true) == 2);
            CHECK(hud[0].late_aim && !hud[1].reticle && !hud[1].late_aim && hud[2].late_aim);
            std::vector<gx::DrawPacket> icon_only{quad(0x80393B0C, nx, ny, 100)};
            CHECK(gx::mark_reticle_draws(icon_only, Aims{aim, std::nullopt}, 0, true) == 0);
        }
        {   // Reticles exactly on top of each other: the tint and the draw order still keep the quads apart.
            std::vector<gx::DrawPacket> stacked{tinted(quad(0x80393B0C, nx, ny, 120), red), quad(0x80393B0C, nx, ny, 120), tinted(quad(0x80393B0C, nx + 0.01f, ny, 120), blue), quad(0x80393B0C, nx + 0.01f, ny, 120)};
            CHECK(mark(stacked, {aim, aim}, 0) == 4);
            CHECK(stacked[0].late_aim && stacked[1].late_aim && !stacked[2].late_aim && !stacked[3].late_aim && stacked[2].reticle && stacked[3].reticle);
        }
        std::vector<gx::DrawPacket> far{tinted(quad(0x80393B0C, nx - 0.45f, ny, 120), red)};   // a fast flick: 0.45 from the pointer it was drawn from
        CHECK(mark(far, {aim, other}, 0) == 1 && far[0].late_aim);
    }
    {   // The pointer's range is wider than the screen (1.25 x 1.67), the game keeps its own ring on the screen edge (NDC +-1): the reticle follows the pointer only that far.
        const auto edge = gx::clamp_aim({1.f, -1.f});
        CHECK(std::abs(edge[0] - 0.8f) < 1e-5f && std::abs(edge[1] + 0.6f) < 1e-5f);
        const auto inside = gx::clamp_aim({0.3f, -0.2f});
        CHECK(inside[0] == 0.3f && inside[1] == -0.2f);
        std::vector<gx::DrawPacket> corner{tinted(quad(0x80393B0C, 1.f, 1.f, 154), red)};   // the game's ring at the corner while the pointer is at the end of its range
        CHECK(mark(corner, {std::array<float,2>{1.f, 1.f}, std::nullopt}, 0) == 1 && corner[0].late_aim);   // the vertical gap (1.67 - 1) must not read as "another ring"
    }
    {   // The wake (callsite 8006CED4, in the player's tint) follows only the local reticle and is not scaled.
        std::vector<gx::DrawPacket> draws{tinted(quad(0x8006CED4, 0.f, 0.f, 100), red), tinted(quad(0x8006CED4, 0.f, 0.f, 100), blue), quad(0x8006CED4, 0.f, 0.f, 100)};
        const std::array<float,2> other{0.5f, -0.2f};
        CHECK(gx::mark_reticle_draws(draws, Aims{aim, other}, 1, true) == 1 && !draws[0].late_aim && draws[1].late_aim && !draws[2].late_aim && !draws[1].reticle);
        std::vector<gx::DrawPacket> single{tinted(quad(0x8006CED4, 0.f, 0.f, 100), blue)};
        CHECK(gx::mark_reticle_draws(single, Aims{aim, std::nullopt}, 0, true) == 1 && single[0].late_aim);
        auto faded = tinted(quad(0x8006CED4, 0.f, 0.f, 100), blue); faded.vertices[0].color[0] = {255, 255, 255, 0};   // the strip's head is faded: a later vertex names the owner
        std::vector<gx::DrawPacket> two{faded};
        CHECK(gx::mark_reticle_draws(two, Aims{aim, other}, 1, true) == 1 && two[0].late_aim && two[0].wake);
        std::vector<gx::DrawPacket> solo{tinted(quad(0x8006CED4, 0.f, 0.f, 100), red)};   // not exact: the game's own wake stays put
        CHECK(mark(solo, {aim, std::nullopt}, 0) == 0 && !solo[0].late_aim && !solo[0].wake);
        std::vector<gx::DrawPacket> plain{quad(0x8006CED4, 0.f, 0.f, 100)};               // single-player (exact): a wake without a tint is the lone pad's
        CHECK(gx::mark_reticle_draws(plain, Aims{aim, std::nullopt}, 0, true) == 1 && plain[0].late_aim && plain[0].wake);
    }
    {   // The wake strip is laid along the mouse path: centres on it, half-width and side kept, pairs turned square to the path.
        std::vector<std::array<float,2>> strip;   // an old strip along +y (older pairs lower), 0.02 half-width on the right
        for (int k = 0; k < 10; ++k) { strip.push_back({0.1f - 0.02f, -0.5f + 0.05f * k}); strip.push_back({0.1f + 0.02f, -0.5f + 0.05f * k}); }
        const auto path = [](double age) { return std::array<float,2>{float(0.5 - age * 2.0), 0.25f}; };   // the mouse moved left along y = 0.25
        const auto laid = gx::wake_strip_ndc(strip, 0.9, path);
        CHECK(laid.size() == strip.size());
        for (size_t k = 0; k < 10; ++k) {
            const float cx = (laid[2*k][0] + laid[2*k+1][0]) / 2, cy = (laid[2*k][1] + laid[2*k+1][1]) / 2;
            CHECK(std::abs(cx - (0.5f - 2.0f * 0.9f * k / 9)) < 1e-4f && std::abs(cy - 0.25f) < 1e-4f);
            CHECK(std::abs(laid[2*k+1][0] - laid[2*k][0]) < 1e-4f);                                    // the width runs along y now
            CHECK(std::abs(std::hypot(laid[2*k+1][0] - laid[2*k][0], laid[2*k+1][1] - laid[2*k][1]) - 0.04f) < 1e-4f);
        }
        CHECK(laid[1][1] > laid[0][1] || laid[1][1] < laid[0][1]);
        std::vector<std::array<float,2>> tiny{{0,0},{0.1f,0}};
        CHECK(gx::wake_strip_ndc(tiny, 1.0, path) == tiny);                                            // too short to be a strip: untouched
    }
    // Crosshair scale: the viewport shrinks about the quad's centre, which keeps its pixel (pixel = x + (ndc * 0.5 + 0.5) * width).
    float x = 10, y = 20, width = 1280, height = 720;
    gx::scale_viewport_about({nx, ny}, 0.5f, x, y, width, height);
    CHECK(width == 640 && height == 360);
    CHECK(std::abs(x + (nx * .5f + .5f) * width - (10 + (nx * .5f + .5f) * 1280)) < 1e-3f);
    CHECK(std::abs(y + (ny * .5f + .5f) * height - (20 + (ny * .5f + .5f) * 720)) < 1e-3f);
    {   // The reticle style (the menu's Reticle tab): parts the player hid are dropped, the circle takes the colour at the game's brightness, the dot grows about its own centre.
        const auto circle = [&] {   // a 120 texture with ink on its diagonals: the circle (the marks' textures have none)
            auto draw = tinted(quad(0x80393B0C, nx, ny, 120), red);
            auto texture = std::make_shared<gx::TexturePixels>();
            texture->levels.push_back({120, 120, std::vector<uint8_t>(120 * 120 * 4, 0)});
            for (const auto [x, y] : {std::pair{87, 32}, std::pair{32, 32}, std::pair{32, 87}, std::pair{87, 87}}) texture->levels[0].rgba[(size_t(y) * 120 + size_t(x)) * 4 + 3] = 255;
            draw.texture = texture;
            return draw;
        };
        const auto reticle = [&] { return std::vector<gx::DrawPacket>{circle(), quad(0x80393B0C, nx, ny, 120), quad(0x80393B0C, nx, ny, 154), quad(0x8006D174, nx, ny, 100), tinted(quad(0x8006CED4, 0.f, 0.f, 100), red)}; };
        const auto parts = [](const std::vector<gx::DrawPacket>& draws) { std::vector<gx::ReticlePart> out; for (const auto& d : draws) out.push_back(gx::reticle_part(d)); return out; };
        auto draws = reticle();
        CHECK(mark(draws, {aim, std::nullopt}, 0) == 4);
        CHECK((parts(draws) == std::vector<gx::ReticlePart>{gx::ReticlePart::ring, gx::ReticlePart::marks, gx::ReticlePart::arc, gx::ReticlePart::dot, gx::ReticlePart::wake}));
        gx::ReticleStyle plain;
        auto untouched = draws; gx::apply_reticle_style(untouched, plain, 0, false);
        CHECK(untouched.size() == 5 && plain.plain());
        gx::ReticleStyle hide; hide.hide_tail = hide.hide_marks = true;
        auto hidden = draws; gx::apply_reticle_style(hidden, hide, 0, false);
        CHECK((parts(hidden) == std::vector<gx::ReticlePart>{gx::ReticlePart::ring, gx::ReticlePart::arc, gx::ReticlePart::dot}));
        gx::ReticleStyle hide_rest; hide_rest.hide_dot = hide_rest.hide_arc = true;
        auto hidden_rest = draws; gx::apply_reticle_style(hidden_rest, hide_rest, 0, false);
        CHECK((parts(hidden_rest) == std::vector<gx::ReticlePart>{gx::ReticlePart::ring, gx::ReticlePart::marks, gx::ReticlePart::wake}));
        gx::ReticleStyle look; look.color = {0x00FF00, 0}; look.dot_scale = 2;
        auto styled = draws; gx::apply_reticle_style(styled, look, 0, false);
        CHECK(styled[0].vertices[0].color[0][0] == 0 && styled[0].vertices[0].color[0][1] == 255 && styled[0].vertices[0].color[0][2] == 0 && styled[0].vertices[0].color[0][3] == 255);   // the circle: green
        CHECK(styled[4].vertices[0].color[0][1] == 255 && styled[4].vertices[0].color[0][0] == 0);                                                                                           // the wake too
        CHECK(styled[1].vertices[0].color[0] == draws[1].vertices[0].color[0] && styled[2].vertices[0].color[0] == draws[2].vertices[0].color[0]);                                             // marks and arc keep their own
        CHECK(std::abs(styled[3].vertices[0].position[0] - (nx - 0.16f)) < 1e-5f && std::abs(styled[3].vertices[2].position[1] - (ny + 0.16f)) < 1e-5f);                                      // the dot: twice the size about its centre
        gx::ReticleStyle dark; dark.color = {0x400000, 0};
        auto darkened = draws; gx::apply_reticle_style(darkened, dark, 0, false);
        CHECK(darkened[0].vertices[0].color[0][0] == 64 && darkened[0].vertices[0].color[0][1] == 0);   // a dark pick stays dark
        auto dimmed = draws; dimmed[2].late_aim = false;   // a second pad exists and this arc is not the local player's
        gx::ReticleStyle fade; fade.partner_alpha = 0.5f;
        gx::apply_reticle_style(dimmed, fade, 0, true);
        CHECK(dimmed[2].vertices[0].color[0][3] == 128 && dimmed[0].vertices[0].color[0][3] == 255);
        auto lone = draws; lone[2].late_aim = false; gx::apply_reticle_style(lone, fade, 0, false);
        CHECK(lone[2].vertices[0].color[0][3] == 255);   // one pad: nobody is a partner
    }
    return 0;
}
