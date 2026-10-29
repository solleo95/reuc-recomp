#include "check.h"
#include "ultrawide.h"
#include <cmath>
// Native ultrawide: the game builds the wide 3D picture itself; the packets' orthographic draws are narrowed by 16:9 / aspect (aspect / 16:9 for y): about the screen
// centre in a menu, about their cluster's centre in a frame with a 3D scene (a player's block with a health bar excepted); the game's scratch passes, the draws
// across the whole axis and the perspective draws stay as they are.
static gx::DrawPacket ortho_quad(float left, float right, const std::shared_ptr<const gx::Transforms>& shared = nullptr, float bottom = -0.5f, float top = 0.5f) {
    gx::DrawPacket draw;
    gx::Transforms t; t.projection = {{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};   // vertices are given directly in NDC
    draw.transforms = shared ? gx::Shared<gx::Transforms>(shared) : gx::Shared<gx::Transforms>(t);
    gx::State state; draw.state = state;
    draw.vertices.resize(4);
    draw.vertices[0].position = {left, bottom, 0}; draw.vertices[1].position = {right, bottom, 0};
    draw.vertices[2].position = {right, top, 0}; draw.vertices[3].position = {left, top, 0};
    return draw;
}
static gx::DrawPacket plate(float left, float right, float bottom, float top) {   // the HUD panel behind a player's name, bar, weapon and counts: a 220x64 texture
    auto draw = ortho_quad(left, right, nullptr, bottom, top);
    auto texture = std::make_shared<gx::TexturePixels>();
    texture->desc_width = 220; texture->desc_height = 64;
    draw.texture = texture;
    return draw;
}
static gx::DrawPacket scene_draw() {
    gx::DrawPacket draw;
    gx::Transforms t; t.projection = {{{1.2f,0,0,0},{0,1.6f,0,0},{0,0,-1,-1},{0,0,-1,0}}};   // GX perspective: w = -z
    draw.transforms = t;
    gx::State state; draw.state = state;
    return draw;
}
static bool near(float a, float b) { return std::abs(a - b) < 1e-5f; }
static std::array<float,2> x_range(const gx::DrawPacket& draw) { return *gx::ortho_range(draw, 0); }
static std::array<float,2> y_range(const gx::DrawPacket& draw) { return *gx::ortho_range(draw, 1); }
static float centre(std::array<float,2> range) { return (range[0] + range[1]) / 2; }
static float width(std::array<float,2> range) { return range[1] - range[0]; }
int main() {
    {   // The factors of an output shape: 16:9 and its neighbours change nothing.
        const auto same = gx::factors_for_aspect(16.0 / 9), wide = gx::factors_for_aspect(21.0 / 9, true), super = gx::factors_for_aspect(32.0 / 9), tall = gx::factors_for_aspect(16.0 / 10, true);
        CHECK(same.x == 1.f && same.y == 1.f && !same.edges);
        CHECK(near(wide.x, 16.f / 21.f) && wide.y == 1.f && wide.edges);
        CHECK(near(super.x, 0.5f) && super.y == 1.f && !super.edges);
        CHECK(tall.x == 1.f && near(tall.y, 1.6f / (16.f / 9)) && !tall.edges);   // edges only on a wider output
        CHECK(gx::factors_for_aspect(0).x == 1.f && gx::factors_for_aspect(0).y == 1.f);
    }
    const gx::ViewFactors wide{0.75f, 1.f, false}, edges{0.75f, 1.f, true};
    {   // A menu (no 3D scene): everything narrows about the screen centre; the perspective draw is left to the game.
        gx::FramePacket packet;
        packet.draws.push_back(ortho_quad(-0.9f, -0.2f)); packet.draws.push_back(ortho_quad(0.2f, 0.4f));
        gx::WideFrame scene;
        gx::widen_packet(packet, wide, scene);
        CHECK(near(x_range(packet.draws[0])[0], 0.75f * -0.9f) && near(x_range(packet.draws[0])[1], 0.75f * -0.2f) && near(x_range(packet.draws[1])[0], 0.75f * 0.2f));
        CHECK(y_range(packet.draws[0])[0] == -0.5f);
        gx::FramePacket three_d; three_d.draws.push_back(scene_draw());
        gx::widen_packet(three_d, wide, scene);
        CHECK(three_d.draws[0].transforms->projection[0][0] == 1.2f && !scene.scene);   // a presenting packet ends the frame
    }
    {   // A draw across the whole width stays stretched (a background, a fade); the game's scratch passes (a packet ending in a wide EFB copy) stay as they are.
        gx::FramePacket packet; packet.draws.push_back(ortho_quad(-1, 1)); packet.draws.push_back(ortho_quad(-0.3f, 0.3f)); packet.draws.push_back(ortho_quad(-1.2f, 1.2f));
        gx::WideFrame scene;
        gx::widen_packet(packet, wide, scene);
        CHECK(packet.draws[0].transforms->projection[0][0] == 1.f && near(packet.draws[1].transforms->projection[0][0], 0.75f) && packet.draws[2].transforms->projection[0][0] == 1.f);
        gx::FramePacket copy; copy.present = false; copy.texture_copy = gx::TextureCopy{}; copy.texture_copy->source = {0, 0, 640, 360};
        copy.draws.push_back(ortho_quad(-1, 0));
        gx::widen_packet(copy, wide, scene);
        CHECK(copy.draws[0].transforms->projection[0][0] == 1.f);
    }
    {   // A frame with a 3D scene: a marker over an object (not at the screen centre) keeps its centre and loses only the stretch, so it stays on the object.
        gx::FramePacket packet; packet.draws.push_back(scene_draw()); packet.draws.push_back(ortho_quad(-0.62f, -0.42f, nullptr, 0.0f, 0.2f));
        packet.draws.push_back(ortho_quad(0.5f, 0.9f, nullptr, -0.5f, -0.3f));
        gx::WideFrame scene;
        gx::widen_packet(packet, wide, scene);
        CHECK(near(centre(x_range(packet.draws[1])), -0.52f) && near(width(x_range(packet.draws[1])), 0.75f * 0.2f));
        CHECK(near(centre(x_range(packet.draws[2])), 0.7f) && near(width(x_range(packet.draws[2])), 0.75f * 0.4f));
        CHECK(near(y_range(packet.draws[1])[0], 0.0f) && near(y_range(packet.draws[1])[1], 0.2f));
    }
    {   // The glyphs of a line of text are one cluster: they narrow together about the line's centre, so their spacing narrows too.
        gx::FramePacket packet; packet.draws.push_back(scene_draw());
        packet.draws.push_back(ortho_quad(-0.30f, -0.20f, nullptr, -0.8f, -0.7f)); packet.draws.push_back(ortho_quad(-0.19f, -0.09f, nullptr, -0.8f, -0.7f));
        packet.draws.push_back(ortho_quad(-0.08f, 0.02f, nullptr, -0.8f, -0.7f));
        gx::WideFrame scene;
        gx::widen_packet(packet, wide, scene);
        const float c = -0.14f;   // the line's centre
        CHECK(near(x_range(packet.draws[1])[0], c + 0.75f * (-0.30f - c)) && near(x_range(packet.draws[3])[1], c + 0.75f * (0.02f - c)));
        CHECK(near(x_range(packet.draws[2])[0] - x_range(packet.draws[1])[1], 0.75f * 0.01f));   // the gap between glyphs narrowed too
    }
    {   // A player's block (what lies on its plate): with `edges` it keeps its margin from the screen edge, otherwise its centre moves in by k. The reticle keeps its centre.
        const auto make = [&] {
            gx::FramePacket packet; packet.draws.push_back(scene_draw());
            packet.draws.push_back(plate(-0.95f, -0.3f, -0.8f, -0.45f));
            packet.draws.push_back(ortho_quad(-0.9f, -0.72f, nullptr, -0.72f, -0.47f));   // the weapon, on it
            packet.draws.push_back(ortho_quad(-0.1f, 0.1f, nullptr, -0.1f, 0.1f));        // the reticle
            packet.draws.back().reticle = true;
            return packet;
        };
        auto at_edges = make(); gx::WideFrame scene;
        gx::widen_packet(at_edges, edges, scene);
        CHECK(near(x_range(at_edges.draws[1])[0], -0.95f) && near(width(x_range(at_edges.draws[1])), 0.75f * 0.65f) && near(x_range(at_edges.draws[2])[0], -0.95f + 0.75f * 0.05f));   // the margin from the left edge kept
        auto in_middle = make(); scene = {};
        gx::widen_packet(in_middle, wide, scene);
        CHECK(near(x_range(in_middle.draws[1])[0], 0.75f * -0.95f) && near(centre(x_range(in_middle.draws[1])), 0.75f * -0.625f));   // the 16:9 place
        CHECK(near(centre(x_range(at_edges.draws[3])), 0.f) && near(width(x_range(at_edges.draws[3])), 0.75f * 0.2f));
        CHECK(near(centre(x_range(in_middle.draws[3])), 0.f));
    }
    {   // Text is no player block, whatever draws it (the font's callsite draws every text of the screen): a message in the middle keeps its centre.
        gx::FramePacket packet; packet.draws.push_back(scene_draw()); packet.draws.push_back(plate(-0.95f, -0.3f, -0.8f, -0.45f));
        packet.draws.push_back(ortho_quad(-0.2f, 0.2f, nullptr, 0.0f, 0.1f)); packet.draws.back().callsite = 0x803B7FE4;
        gx::WideFrame frame;
        gx::widen_packet(packet, edges, frame);
        CHECK(near(centre(x_range(packet.draws[2])), 0.f) && near(width(x_range(packet.draws[2])), 0.75f * 0.4f));
    }
    {   // A render-to-texture pass (a copy narrower than the picture) is no 3D scene: the frame stays a menu, the draws narrow about the screen centre.
        gx::FramePacket preview; preview.present = false; preview.texture_copy = gx::TextureCopy{}; preview.texture_copy->source = {0, 0, 200, 200};
        preview.draws.push_back(scene_draw());
        gx::WideFrame scene;
        gx::widen_packet(preview, edges, scene);
        CHECK(!scene.scene);
        gx::FramePacket menu; menu.draws.push_back(ortho_quad(-0.62f, -0.42f));
        gx::widen_packet(menu, edges, scene);
        CHECK(near(x_range(menu.draws[0])[0], 0.75f * -0.62f));
    }
    {   // The 3D scene may come in an earlier packet of the frame: the interface packet after it still counts as a 3D frame.
        gx::FramePacket first; first.present = false; first.draws.push_back(scene_draw());
        gx::WideFrame scene;
        gx::widen_packet(first, wide, scene);
        CHECK(scene.scene);
        gx::FramePacket hud; hud.draws.push_back(ortho_quad(-0.62f, -0.42f));
        gx::widen_packet(hud, wide, scene);
        CHECK(near(centre(x_range(hud.draws[0])), -0.52f) && !scene.scene);
    }
    {   // Draws sharing a transform block share the narrowed block when they narrow alike; the original block is not changed.
        gx::FramePacket packet;
        packet.draws.push_back(ortho_quad(-0.5f, 0.5f));
        const auto original = packet.draws[0].transforms.block();
        packet.draws.push_back(ortho_quad(-0.4f, 0.4f, original));
        gx::WideFrame scene;
        gx::widen_packet(packet, wide, scene);
        CHECK(packet.draws[0].transforms.block() == packet.draws[1].transforms.block());
        CHECK(original->projection[0][0] == 1.f);
    }
    {   // A taller output narrows the y axis the same way; a draw across the whole height stays stretched.
        const gx::ViewFactors tall{1.f, 0.9f, false};
        gx::FramePacket packet;
        packet.draws.push_back(ortho_quad(-0.5f, 0.5f)); packet.draws.push_back(ortho_quad(-0.5f, 0.5f, nullptr, -1, 1));
        gx::WideFrame scene;
        gx::widen_packet(packet, tall, scene);
        CHECK(near(y_range(packet.draws[0])[0], 0.9f * -0.5f) && near(x_range(packet.draws[0])[0], -0.5f) && packet.draws[1].transforms->projection[1][1] == 1.f);
    }
    {   // Full-screen pictures (a 640x480 texture across the whole width): a still one keeps 4:3, a movie 16:9, each clipped to its own area; not in a frame with a 3D scene.
        const auto picture = [](uint32_t callsite, uint32_t width, uint32_t height) {
            auto draw = ortho_quad(-1.1f, 1.1f);
            draw.callsite = callsite;
            auto texture = std::make_shared<gx::TexturePixels>();
            texture->desc_width = width; texture->desc_height = height;
            draw.texture = texture;
            return draw;
        };
        gx::FramePacket packet;
        packet.draws.push_back(picture(0x80393B0C, 640, 480)); packet.draws.push_back(picture(gx::kMovieCallsite, 640, 480)); packet.draws.push_back(picture(0x80393B0C, 256, 256));
        gx::WideFrame scene;
        gx::widen_packet(packet, wide, scene);
        CHECK(near(packet.draws[0].transforms->projection[0][0], 0.75f * 0.75f) && near(packet.draws[1].transforms->projection[0][0], 0.75f) && packet.draws[2].transforms->projection[0][0] == 1.f);
        const auto& still = packet.draws[0].state->scissor;
        CHECK(still && (*still)[0] == int32_t(std::ceil(320 * (1 - 0.5625f))) && (*still)[0] + (*still)[2] == int32_t(std::floor(320 * (1 + 0.5625f))) && (*still)[3] == 480);
        const auto& movie = packet.draws[1].state->scissor;
        CHECK(movie && (*movie)[0] == 80 && (*movie)[0] + (*movie)[2] == 560);
        CHECK(!packet.draws[2].state->scissor);
        gx::FramePacket in_scene; in_scene.present = false; in_scene.draws.push_back(scene_draw());
        gx::widen_packet(in_scene, wide, scene);
        gx::FramePacket later; later.draws.push_back(picture(0x80393B0C, 640, 480));
        gx::widen_packet(later, wide, scene);
        CHECK(later.draws[0].transforms->projection[0][0] == 1.f);   // a picture in a frame with a scene is left stretched
    }
    {   // 16:9: nothing changes.
        gx::FramePacket packet; packet.draws.push_back(ortho_quad(-0.5f, 0.5f));
        const auto before = packet.draws[0].transforms.block();
        gx::WideFrame scene;
        gx::widen_packet(packet, {}, scene);
        CHECK(packet.draws[0].transforms.block() == before);
    }
    {   // The counts and the bar's ends lie on the plate and belong to the block, however far the bar reaches.
        gx::FramePacket packet; packet.draws.push_back(scene_draw());
        packet.draws.push_back(plate(-0.95f, -0.3f, -0.8f, -0.45f));
        packet.draws.push_back(ortho_quad(-0.35f, -0.27f, nullptr, -0.6f, -0.5f));   // the grenade count, at the plate's right end
        packet.draws.push_back(ortho_quad(-0.97f, -0.93f, nullptr, -0.78f, -0.7f)); // the bar's cap, left of the first segment
        gx::WideFrame frame;
        gx::widen_packet(packet, edges, frame);
        CHECK(near(x_range(packet.draws[2])[0], -0.95f + 0.75f * 0.6f) && near(x_range(packet.draws[3])[0], -0.95f + 0.75f * -0.02f));   // narrowed towards the left edge together
    }
    {   // The count comes in another packet than the plate (the game draws them apart): it still joins the block, which narrows alike.
        gx::FramePacket first; first.present = false; first.draws.push_back(scene_draw());
        first.draws.push_back(plate(-0.95f, -0.3f, -0.8f, -0.45f));
        gx::WideFrame frame;
        gx::widen_packet(first, edges, frame);
        gx::FramePacket second; second.draws.push_back(ortho_quad(-0.35f, -0.27f, nullptr, -0.6f, -0.5f));
        gx::widen_packet(second, edges, frame);
        CHECK(near(x_range(second.draws[0])[0], -0.95f + 0.75f * 0.6f));
        CHECK(frame.players.empty() && frame.boxes.empty());   // the frame ended with the present
    }
    {   // A large panel (the black band behind a tutorial text) does not bridge the clusters it touches; the text keeps its centre.
        gx::FramePacket packet; packet.draws.push_back(scene_draw());
        packet.draws.push_back(ortho_quad(-0.98f, 0.98f, nullptr, -0.3f, 0.5f));                       // the panel: nearly the whole width
        packet.draws.push_back(ortho_quad(-0.4f, 0.4f, nullptr, -0.1f, 0.0f));                         // a text line, centred
        packet.draws.push_back(ortho_quad(0.9f, 0.95f, nullptr, 0.2f, 0.3f));                          // the arrow at the right
        gx::WideFrame frame;
        gx::widen_packet(packet, wide, frame);
        CHECK(near(centre(x_range(packet.draws[2])), 0.f) && near(centre(x_range(packet.draws[3])), 0.925f));
    }
    {   // A player's block is what lies on its plate, not what chains to it: a wide strip next to it (a hit flash) does not drag it.
        gx::FramePacket packet; packet.draws.push_back(scene_draw());
        packet.draws.push_back(plate(-0.95f, -0.3f, -0.8f, -0.45f));
        packet.draws.push_back(ortho_quad(-0.9f, -0.72f, nullptr, -0.72f, -0.5f));          // the weapon
        packet.draws.push_back(ortho_quad(-0.3f, 0.6f, nullptr, -0.62f, -0.5f));            // a strip touching the plate's row
        gx::WideFrame frame;
        gx::widen_packet(packet, edges, frame);
        CHECK(near(x_range(packet.draws[1])[0], -0.95f) && near(x_range(packet.draws[2])[0], -0.95f + 0.75f * 0.05f));
        CHECK(near(centre(x_range(packet.draws[3])), 0.15f));                                // the strip keeps its place
    }
    {   // Two players' plates: each keeps its own edge margin (the right one is not taken for the left block).
        gx::FramePacket packet; packet.draws.push_back(scene_draw());
        packet.draws.push_back(plate(-0.95f, -0.3f, -0.8f, -0.45f));
        packet.draws.push_back(plate(0.3f, 0.95f, -0.8f, -0.45f));
        gx::WideFrame frame;
        gx::widen_packet(packet, edges, frame);
        CHECK(near(x_range(packet.draws[1])[0], -0.95f) && near(x_range(packet.draws[2])[1], 0.95f));
    }
    {   // The pointer's draws narrow about the reticle's centre: the dot stays in the ring and the wake keeps its end at the reticle.
        gx::FramePacket packet; packet.draws.push_back(scene_draw());
        packet.draws.push_back(ortho_quad(0.28f, 0.47f, nullptr, -0.1f, 0.1f)); packet.draws[1].reticle = true;        // the ring, centred at 0.375
        packet.draws.push_back(ortho_quad(0.35f, 0.40f, nullptr, -0.02f, 0.02f)); packet.draws[2].late_aim = true;    // the dot
        packet.draws.push_back(ortho_quad(-0.1f, 0.35f, nullptr, -0.03f, 0.03f)); packet.draws[3].wake = true;        // the wake, ending at the reticle
        packet.draws.push_back(ortho_quad(0.0f, 0.05f, nullptr, -0.02f, 0.02f)); packet.draws[4].late_aim = true;    // a dot trailing far behind the ring in a fast flick
        gx::WideFrame frame;
        gx::widen_packet(packet, wide, frame);
        CHECK(near(centre(x_range(packet.draws[4])), 0.025f));   // every pointer draw keeps its own centre: the renderer moves each by it to the mouse
        CHECK(near(centre(x_range(packet.draws[1])), 0.375f) && near(centre(x_range(packet.draws[2])), 0.375f));
        CHECK(near(x_range(packet.draws[3])[0], -0.1f) && near(x_range(packet.draws[3])[1], 0.35f));   // the wake is a path of true pointer positions: not narrowed
        packet.draws[3].wake = false; packet.draws[3].callsite = 0x8006CED4;                            // single-player: only the callsite names it
        gx::FramePacket again; again.draws.push_back(scene_draw()); again.draws.push_back(packet.draws[1]); again.draws.push_back(packet.draws[3]);
        gx::WideFrame fresh;
        gx::widen_packet(again, wide, fresh);
        CHECK(near(x_range(again.draws[2])[0], -0.1f) && near(x_range(again.draws[2])[1], 0.35f));
    }
    {   // A full-screen image the game draws in tiles (the screen effects: four quarters that meet in the middle) stays stretched as a whole; a lone half stays narrowed.
        const auto tiles = [&](bool with_scene) {
            gx::FramePacket packet;
            if (with_scene) packet.draws.push_back(scene_draw());
            packet.draws.push_back(ortho_quad(-1, 0, nullptr, -1, 0)); packet.draws.push_back(ortho_quad(0, 1, nullptr, -1, 0));
            packet.draws.push_back(ortho_quad(-1, 0, nullptr, 0, 1)); packet.draws.push_back(ortho_quad(0, 1, nullptr, 0, 1));
            gx::WideFrame frame;
            gx::widen_packet(packet, wide, frame);
            return packet;
        };
        for (const bool with_scene : {false, true}) {
            const auto packet = tiles(with_scene);
            for (size_t i = with_scene ? 1 : 0; i < packet.draws.size(); ++i) CHECK(packet.draws[i].transforms->projection[0][0] == 1.f);
        }
        {   // The tiles of a screen effect lie over the whole screen: what the game draws over them (the health bar, a subtitle) is not part of the image.
            gx::FramePacket packet;
            packet.draws.push_back(scene_draw());
            packet.draws.push_back(ortho_quad(-1, 0, nullptr, -1, 0)); packet.draws.push_back(ortho_quad(0, 1, nullptr, -1, 0));
            packet.draws.push_back(ortho_quad(-1, 0, nullptr, 0, 1)); packet.draws.push_back(ortho_quad(0, 1, nullptr, 0, 1));
            packet.draws.push_back(ortho_quad(-0.95f, -0.3f, nullptr, -0.8f, -0.75f));   // the bar of a player's block: 0.65 wide, inside the screen
            packet.draws.push_back(ortho_quad(-0.5f, 0.5f, nullptr, -0.3f, -0.2f));      // a wide line of text in the middle
            gx::WideFrame frame;
            gx::widen_packet(packet, wide, frame);
            for (size_t i = 1; i <= 4; ++i) CHECK(packet.draws[i].transforms->projection[0][0] == 1.f);
            CHECK(packet.draws[5].transforms->projection[0][0] < 1.f && packet.draws[6].transforms->projection[0][0] < 1.f);
        }
        gx::FramePacket half; half.draws.push_back(ortho_quad(-1, 0, nullptr, -1, 0)); half.draws.push_back(ortho_quad(0.1f, 0.3f));
        gx::WideFrame frame;
        gx::widen_packet(half, wide, frame);
        CHECK(near(half.draws[0].transforms->projection[0][0], 0.75f));
        gx::FramePacket panels; panels.draws.push_back(ortho_quad(-1, -0.2f, nullptr, -1, 0)); panels.draws.push_back(ortho_quad(0.5f, 0.9f, nullptr, -1, 0));   // two panels with a gap: not one image
        gx::widen_packet(panels, wide, frame);
        CHECK(near(panels.draws[0].transforms->projection[0][0], 0.75f) && near(panels.draws[1].transforms->projection[0][0], 0.75f));
    }
    {   // A menu's reticle (no 3D scene in the frame) keeps its centre as well: the renderer moves it to the mouse, which may be anywhere in the window.
        gx::FramePacket packet;
        packet.draws.push_back(ortho_quad(0.28f, 0.47f, nullptr, -0.1f, 0.1f)); packet.draws[0].reticle = true;   // the ring, centred at 0.375
        packet.draws.push_back(ortho_quad(0.2f, 0.4f));                                                            // a menu text: narrowed about the screen centre
        gx::WideFrame frame;
        gx::widen_packet(packet, wide, frame);
        CHECK(near(centre(x_range(packet.draws[0])), 0.375f) && near(width(x_range(packet.draws[0])), 0.75f * 0.19f));
        CHECK(near(centre(x_range(packet.draws[1])), 0.75f * 0.3f));
    }
    {   // A button prompt takes the narrowing of the text line it stands in (the line came in an earlier packet of the frame), a menu's prompt the screen centre's.
        gx::FramePacket text; text.present = false; text.draws.push_back(scene_draw());
        text.draws.push_back(ortho_quad(0.1f, 0.3f, nullptr, 0.4f, 0.5f)); text.draws.push_back(ortho_quad(0.31f, 0.5f, nullptr, 0.4f, 0.5f));   // a line centred at 0.3
        gx::WideFrame frame;
        gx::widen_packet(text, wide, frame);
        gx::FramePacket present; present.display_source = {0, 0, 640, 480};
        gx::PromptDraw prompt; prompt.efb_corners = {320 * (1 + 0.2f), 240 * (1 + 0.4f), 320 * (1 + 0.28f), 240 * (1 + 0.4f), 320 * (1 + 0.28f), 240 * (1 + 0.5f), 320 * (1 + 0.2f), 240 * (1 + 0.5f)};
        prompt.clip = {320 * (1 + 0.1f), 240 * (1 + 0.4f), 320 * (1 + 0.5f), 240 * (1 + 0.5f)};
        present.prompts.push_back(prompt);
        gx::widen_packet(present, wide, frame);
        const float c = 0.3f, expected = c + 0.75f * (0.2f - c);   // the line's centre is 0.3
        CHECK(near(present.prompts[0].efb_corners[0], 320 * (1 + expected)) && near(present.prompts[0].efb_corners[1], 240 * (1 + 0.4f)));
        CHECK(near(present.prompts[0].clip[0], 320 * (1 + c + 0.75f * (0.1f - c))));
        gx::FramePacket menu; menu.display_source = {0, 0, 640, 480}; menu.prompts.push_back(prompt);
        gx::WideFrame menu_frame;
        gx::widen_packet(menu, wide, menu_frame);
        CHECK(near(menu.prompts[0].efb_corners[0], 320 * (1 + 0.75f * 0.2f)));
    }
}
