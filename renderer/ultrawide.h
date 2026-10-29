#pragma once
#include "gx_packets.h"
#include "late_aim.h"
#include "wide_view.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <tuple>
#include <vector>

namespace gx {
// Native ultrawide. The renderer stretches the EFB over the output whatever its shape. On a wider (taller) output the game is asked for a wider (taller)
// picture instead: game/intro.cpp widens the aspect of every C_MTXPerspective, so the 3D scene, the pointer's range, the hit tests and the culling all see
// the wide picture, and everything the game lays over the scene at a projected point (pickup icons, markers, glints) is already in the right place.
// What this file does is keep the 2D elements from being stretched: an orthographic draw is narrowed by k = 16:9 / output aspect (aspect / 16:9)
//  - in a frame without a 3D scene (menus, titles): about the screen centre, so the 16:9 layout of the game stays in the middle;
//  - in a frame with a 3D scene: about the centre of its cluster (draws whose rectangles touch: the glyphs of a line, a player's name, bar and weapon),
//    so every element keeps its place and loses only the stretch. A player's block (the cluster with a health bar) is the exception: with `edges` it
//    is narrowed towards its outer edge (the same margin from the screen edge as at 16:9), otherwise its centre moves in by k (the 16:9 place in the middle).
// Left alone: the game's own passes over scratch areas of the EFB (packets that end in an EFB copy, e.g. the bloom chain and composites) and the
// orthographic draws that reach both edges of the scaled axis (backgrounds, fades, flashes, cinematic bars), which stay stretched.
inline bool is_perspective(const Projection& p) { return p[3][3] == 0; }   // GX: w = -z; orthographic keeps w = 1

// The NDC range along projection row `row` (0: x, 1: y) of an orthographic draw's geometry (w = 1); nullopt without vertices, with more than 2048 or
// with an unloaded matrix row, or a non-finite coordinate.
inline std::optional<std::array<float,2>> ortho_range(const DrawPacket& draw, size_t row_of_projection) {
    const auto vertices = draw.vertex_view();
    if (vertices.empty() || vertices.size() > 2048) return std::nullopt;
    const auto& t = *draw.transforms;
    const bool indexed = draw.state->descriptors[0] != Mode::none;
    float low = 1e9f, high = -1e9f;
    for (const auto& vertex : vertices) {
        const uint32_t row = indexed ? vertex.position_matrix : t.current;
        if (row > 61) return std::nullopt;
        const auto& p = vertex.position;
        float view[3];
        for (int i = 0; i < 3; ++i) { const auto& m = t.position_rows[row + i]; view[i] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]; }
        const auto& m = t.projection[row_of_projection];
        const float value = m[0] * view[0] + m[1] * view[1] + m[2] * view[2] + m[3];
        low = std::min(low, value); high = std::max(high, value);
    }
    if (!std::isfinite(low) || !std::isfinite(high)) return std::nullopt;   // a NaN anchor would break the ordering of the cache key
    return std::array<float,2>{low, high};
}
// Whether an orthographic draw's geometry reaches both edges of the axis (NDC within 1% of -1 and +1).
inline bool spans_axis(const DrawPacket& draw, size_t row_of_projection) {
    const auto range = ortho_range(draw, row_of_projection);
    return range && (*range)[0] <= -0.99f && (*range)[1] >= 0.99f;
}
// A full-screen picture: an orthographic draw across the whole width with a texture the size of the EFB (640x480), as the movie player and the still
// screens (the Nintendo and CAPCOM logos) draw them. They keep the shape the game made them in instead of being stretched: a movie is 16:9, a still
// picture 4:3. Not in a frame with a 3D scene.
constexpr uint32_t kMovieCallsite = 0x801619C4;
inline bool full_screen_picture(const DrawPacket& draw) {
    if (is_perspective(draw.transforms->projection) || !draw.texture || !spans_axis(draw, 0)) return false;
    return draw.texture->desc_width == 640 && draw.texture->desc_height == 480;   // known at creation: the decode may still be running
}
// A player's block is found by its plate: the panel (a 220x64 texture) behind the name, the bar, the weapon and the counts. (Callsite 0x803B7FE4 is the
// game's font, which draws every text of the screen: it says nothing about the HUD.)
inline bool is_hud_plate(const DrawPacket& draw) { return draw.texture && draw.texture->desc_width == 220 && draw.texture->desc_height == 64; }
// The aim reticle, its wake and anything else the renderer places by the pointer.
inline bool follows_pointer(const DrawPacket& draw) { return draw.reticle || draw.layer || draw.wake || draw.late_aim; }

// Clusters the movable draws of a packet: draws whose rectangles touch (closer than kGap in NDC) belong together, large ones (wider or taller than 0.9: a
// panel behind a text, a bar across the screen) never bridge two clusters; a thin wide bar (a boss's gauge, at most 0.12 tall) is no panel: it clusters with its caps. A pointer-following draw is a cluster of its own. A player's block is what lies
// around its plate (is_hud_plate): the name, the bar, the weapon and the counts.
inline std::vector<Anchor> cluster_anchors(const FramePacket& packet, const std::vector<bool>& movable, ViewFactors k, const std::vector<PlayerRegion>& known = {},
                                           std::vector<AnchoredBox>* found = nullptr, std::vector<PlayerRegion>* new_regions = nullptr) {
    const size_t count = packet.draws.size();
    std::vector<Anchor> anchors(count);
    struct Box { size_t draw; float x0, x1, y0, y1; bool large, bar, loose; };
    std::vector<Box> boxes;
    for (size_t i = 0; i < count; ++i) {
        if (!movable[i]) continue;
        const auto x = ortho_range(packet.draws[i], 0), y = ortho_range(packet.draws[i], 1);
        if (!x || !y) continue;
        const bool bar = (*x)[1] - (*x)[0] > 0.9f && (*y)[1] - (*y)[0] <= 0.12f;   // a gauge's rail or fill (a boss's): part of the block it joins the caps and the label of
        const bool large = !bar && ((*x)[1] - (*x)[0] > 0.9f || (*y)[1] - (*y)[0] > 0.9f);
        boxes.push_back({i, (*x)[0], (*x)[1], (*y)[0], (*y)[1], large, is_hud_plate(packet.draws[i]), follows_pointer(packet.draws[i])});
    }
    if (boxes.empty() || boxes.size() > 3000) return anchors;
    constexpr float kGap = 0.08f;
    const auto touch = [&](const Box& a, const Box& b) { return a.x0 <= b.x1 + kGap && b.x0 <= a.x1 + kGap && a.y0 <= b.y1 + kGap && b.y0 <= a.y1 + kGap; };
    // Player blocks: a plate (touching plates form one) names the block: the draws whose centre lies on it, whatever packet they come in (the ammo count comes
    // apart from the bar). The block's anchor comes from the plate alone.
    struct Extent { float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f; };
    std::vector<PlayerRegion> regions = known;
    std::vector<int> owner(boxes.size(), -1);
    {
        std::vector<size_t> plates;
        for (size_t i = 0; i < boxes.size(); ++i) if (boxes[i].bar) plates.push_back(i);
        std::vector<int> group(plates.size(), -1);
        std::vector<Extent> plate_extent;
        for (size_t a = 0; a < plates.size(); ++a) {
            if (group[a] < 0) { group[a] = int(plate_extent.size()); plate_extent.emplace_back(); }
            for (size_t b = a + 1; b < plates.size(); ++b) if (touch(boxes[plates[a]], boxes[plates[b]])) group[b] = group[a];
        }
        for (size_t a = 0; a < plates.size(); ++a) {
            auto& e = plate_extent[group[a]]; const auto& b = boxes[plates[a]];
            e.x0 = std::min(e.x0, b.x0); e.x1 = std::max(e.x1, b.x1); e.y0 = std::min(e.y0, b.y0); e.y1 = std::max(e.y1, b.y1);
        }
        for (const auto& e : plate_extent) {
            const bool left = (e.x0 + e.x1) / 2 < 0;
            PlayerRegion region{e.x0 - 0.1f, e.x1 + 0.1f, e.y0 - 0.1f, e.y1 + 0.1f, {}};
            Anchor& anchor = region.anchor;
            anchor.anchored = true;
            anchor.cx = (e.x0 + e.x1) / 2; anchor.cy = (e.y0 + e.y1) / 2;
            anchor.tx = anchor.cx; anchor.ty = anchor.cy;
            if (k.edges) anchor.cx = anchor.tx = left ? e.x0 : e.x1;   // keeps its margin from the screen edge (narrowed towards it)
            else anchor.tx = k.x * anchor.cx;                          // else it takes its 16:9 place
            regions.push_back(region);
            if (new_regions) new_regions->push_back(region);
        }
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].large || boxes[i].loose) continue;
            const float cx = (boxes[i].x0 + boxes[i].x1) / 2, cy = (boxes[i].y0 + boxes[i].y1) / 2;
            for (size_t p = 0; p < regions.size(); ++p) {
                const auto& r = regions[p];
                if (cx >= r.from && cx <= r.to && cy >= r.y0 && cy <= r.y1) { owner[i] = int(p); break; }
            }
        }
    }
    // The rest: clusters by touching.
    std::vector<size_t> parent(boxes.size());
    std::iota(parent.begin(), parent.end(), size_t(0));
    const auto root = [&](size_t i) { while (parent[i] != i) i = parent[i] = parent[parent[i]]; return i; };
    for (size_t a = 0; a < boxes.size(); ++a) {
        if (owner[a] >= 0 || boxes[a].large || boxes[a].loose) continue;
        for (size_t b = a + 1; b < boxes.size(); ++b) {
            if (owner[b] >= 0 || boxes[b].large || boxes[b].loose) continue;
            if (touch(boxes[a], boxes[b])) parent[root(a)] = root(b);
        }
    }
    std::map<size_t, Extent> clusters;
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (owner[i] >= 0 || boxes[i].large || boxes[i].loose) continue;
        auto& cluster = clusters[root(i)];
        cluster.x0 = std::min(cluster.x0, boxes[i].x0); cluster.x1 = std::max(cluster.x1, boxes[i].x1);
        cluster.y0 = std::min(cluster.y0, boxes[i].y0); cluster.y1 = std::max(cluster.y1, boxes[i].y1);
    }
    // The pointer's draws (the ring, the dot, the halo) narrow about their own centres: the renderer later moves each one by its own centre to the mouse
    // (late_aim.h), so a centre that narrowing moved would land off the mouse (the game's dot trails the ring in a fast flick).
    for (size_t i = 0; i < boxes.size(); ++i) {
        Anchor& anchor = anchors[boxes[i].draw];
        if (owner[i] >= 0) anchor = regions[owner[i]].anchor;   // a player's block
        else {
            float x0 = boxes[i].x0, x1 = boxes[i].x1, y0 = boxes[i].y0, y1 = boxes[i].y1;   // a large or pointer-following draw: its own
            if (!boxes[i].large && !boxes[i].loose) { const auto& c = clusters[root(i)]; x0 = c.x0; x1 = c.x1; y0 = c.y0; y1 = c.y1; }
            anchor.anchored = true;
            anchor.cx = anchor.tx = (x0 + x1) / 2; anchor.cy = anchor.ty = (y0 + y1) / 2;
        }
        if (found) found->push_back({boxes[i].x0, boxes[i].x1, boxes[i].y0, boxes[i].y1, anchor});
    }
    return anchors;
}

// The anchor of a button prompt (its rectangle in NDC): the one of a draw of the same frame it touches (the text line it stands in), else its own centre.
// A frame without a 3D scene narrows about the screen centre, as every draw of it does.
inline Anchor prompt_anchor(const WideFrame& frame, float x0, float x1, float y0, float y1) {
    Anchor anchor;
    if (!frame.scene) return anchor;
    anchor.anchored = true;
    float best = -1.f;
    for (const auto& box : frame.boxes) {
        constexpr float kGap = 0.08f;
        if (!(box.x0 <= x1 + kGap && x0 <= box.x1 + kGap && box.y0 <= y1 + kGap && y0 <= box.y1 + kGap)) continue;
        const float overlap = std::max(0.f, std::min(x1, box.x1) - std::max(x0, box.x0)) * std::max(0.f, std::min(y1, box.y1) - std::max(y0, box.y0));
        const float score = overlap + 1e-3f / (1e-3f + std::abs((box.x0 + box.x1) / 2 - (x0 + x1) / 2));   // the most overlap, then the nearest
        if (score > best) { best = score; anchor = box.anchor; }
    }
    if (best < 0) { anchor.cx = anchor.tx = (x0 + x1) / 2; anchor.cy = anchor.ty = (y0 + y1) / 2; }
    return anchor;
}

// A full-screen image the game draws in tiles (the screen effects, backgrounds: quarters or strips that meet) reaches both edges only as a whole. Draws that touch (or
// overlap, within 0.02 NDC) form one piece; a piece that spans the scaled axis stays stretched like a single draw that does.
inline void keep_tiled_images(const FramePacket& packet, std::vector<bool>& movable, const std::vector<int>& picture, ViewFactors k) {
    const size_t axis = k.x < 1.f ? 0 : 1;
    struct Piece { size_t draw; float a0, a1, b0, b1; };
    std::vector<Piece> pieces;
    for (size_t i = 0; i < packet.draws.size() && pieces.size() <= 256; ++i) {
        if (!movable[i] || picture[i]) continue;   // a movie or a still picture is scaled by its own shape, not stretched
        const auto a = ortho_range(packet.draws[i], axis), b = ortho_range(packet.draws[i], 1 - axis);
        if (!a || !b || (*a)[1] - (*a)[0] < 0.4f) continue;
        const bool at_edge = (*a)[0] <= -0.99f || (*a)[1] >= 0.99f || (*b)[0] <= -0.99f || (*b)[1] >= 0.99f;   // a tile of a full-screen image lies on the screen's edge: what is drawn over it (the health bar, a subtitle) does not
        if (at_edge) pieces.push_back({i, (*a)[0], (*a)[1], (*b)[0], (*b)[1]});
    }
    if (pieces.size() < 2 || pieces.size() > 256) return;
    constexpr float kTouch = 0.02f;
    std::vector<size_t> parent(pieces.size());
    std::iota(parent.begin(), parent.end(), size_t(0));
    const auto root = [&](size_t i) { while (parent[i] != i) i = parent[i] = parent[parent[i]]; return i; };
    for (size_t p = 0; p < pieces.size(); ++p) for (size_t q = p + 1; q < pieces.size(); ++q) {
        const auto &u = pieces[p], &v = pieces[q];
        if (u.a0 <= v.a1 + kTouch && v.a0 <= u.a1 + kTouch && u.b0 <= v.b1 + kTouch && v.b0 <= u.b1 + kTouch) parent[root(p)] = root(q);
    }
    std::map<size_t, std::array<float,2>> extent;
    for (size_t p = 0; p < pieces.size(); ++p) {
        auto [it, fresh] = extent.try_emplace(root(p), std::array<float,2>{pieces[p].a0, pieces[p].a1});
        if (!fresh) { it->second[0] = std::min(it->second[0], pieces[p].a0); it->second[1] = std::max(it->second[1], pieces[p].a1); }
    }
    for (size_t p = 0; p < pieces.size(); ++p) {
        const auto& e = extent[root(p)];
        if (e[0] <= -0.99f && e[1] >= 0.99f) movable[pieces[p].draw] = false;
    }
}

// Narrows the packet's orthographic draws and button prompts (see the top of this file). `frame` is the producer's memory of the frame being built: whether it
// already had a perspective draw (the interface comes in a later packet) and the anchored draws so far; it clears at the present.
inline void widen_packet(FramePacket& packet, ViewFactors k, WideFrame& frame) {
    if (k.x >= 1.f && k.y >= 1.f) { if (packet.present) { frame.scene = false; frame.boxes.clear(); frame.players.clear(); } return; }
    // A render-to-texture pass with its own camera (a copy narrower than the picture) is not the scene: it is no 3D frame.
    const bool scene_pass = !packet.texture_copy || packet.texture_copy->source[2] >= 640;
    if (scene_pass) for (const auto& draw : packet.draws) if (is_perspective(draw.transforms->projection)) { frame.scene = true; break; }
    const size_t count = packet.draws.size();
    std::vector<bool> movable(count, false);   // orthographic draws that are narrowed (not the game's scratch passes, not full-axis draws)
    std::vector<int> picture(count, 0);        // 1: a still 4:3 full-screen picture, 2: a movie (16:9): scaled by their own shape, not stretched
    for (size_t at = 0; at < count; ++at) {
        const auto& draw = packet.draws[at];
        if (is_perspective(draw.transforms->projection) || packet.texture_copy) continue;
        if (k.x < 1.f && !frame.scene && full_screen_picture(draw)) { movable[at] = true; picture[at] = draw.callsite == kMovieCallsite ? 2 : 1; continue; }
        // The wake is a ribbon of the pointer's own positions (true NDC already): narrowing it would squeeze its path towards the ring.
        movable[at] = !(draw.wake || reticle_tail_callsite(draw.callsite)) && !(k.x < 1.f ? spans_axis(draw, 0) : spans_axis(draw, 1));
    }
    keep_tiled_images(packet, movable, picture, k);
    std::vector<Anchor> anchors;
    if (frame.scene) {
        std::vector<AnchoredBox> found;
        std::vector<PlayerRegion> fresh;
        anchors = cluster_anchors(packet, movable, k, frame.players, &found, &fresh);
        if (frame.boxes.size() + found.size() <= 6000) frame.boxes.insert(frame.boxes.end(), found.begin(), found.end());
        frame.players.insert(frame.players.end(), fresh.begin(), fresh.end());
    }
    struct Scaled { Shared<Transforms> original, copy; };   // the original stays alive so its address cannot be reused as a key
    std::map<std::tuple<const Transforms*, float, float, float, float, int>, Scaled> scaled;
    for (size_t at = 0; at < count; ++at) {
        auto& draw = packet.draws[at];
        if (is_perspective(draw.transforms->projection) || !movable[at]) continue;   // the 3D scene is wide already: the guest built its projection so
        Anchor anchor;   // a menu: about the screen centre
        if (frame.scene && !picture[at]) { anchor = anchors.empty() ? Anchor{} : anchors[at]; if (!anchor.anchored) continue; }
        else if (!frame.scene && follows_pointer(draw)) {   // a menu's reticle keeps its centre too: the renderer moves it to the mouse, anywhere in the window
            const auto x = ortho_range(draw, 0), y = ortho_range(draw, 1);
            if (x && y) { anchor.anchored = true; anchor.cx = anchor.tx = ((*x)[0] + (*x)[1]) / 2; anchor.cy = anchor.ty = ((*y)[0] + (*y)[1]) / 2; }
        }
        const std::tuple<const Transforms*, float, float, float, float, int> key{draw.transforms.block().get(), anchor.cx, anchor.cy, anchor.tx, anchor.ty, picture[at]};
        auto found = scaled.find(key);
        if (found == scaled.end()) {
            Scaled entry{draw.transforms, draw.transforms};
            auto& projection = entry.copy.edit().projection;
            const float x = picture[at] == 1 ? k.x * 0.75f : k.x;   // 4:3 is 3/4 as wide as 16:9
            for (float& value : projection[0]) value *= x;
            for (float& value : projection[1]) value *= k.y;
            projection[0][3] += anchor.tx - x * anchor.cx;       // orthographic (w = 1): x' = x * cx-scaled + the shift that lands the anchor
            projection[1][3] += anchor.ty - k.y * anchor.cy;
            found = scaled.emplace(key, std::move(entry)).first;
        }
        draw.transforms = found->second.copy;
        if (picture[at]) {   // the game drew the picture a little wider than the screen (-1.1..1.1): its edge cut the rest; here that edge is the picture's own
            const float x = picture[at] == 1 ? k.x * 0.75f : k.x;
            auto& state = draw.state.edit();
            auto scissor = state.scissor.value_or(std::array<int32_t,4>{0, 0, 640, 480});
            const int32_t left = std::max(scissor[0], int32_t(std::ceil(320 * (1 - x)))), right = std::min(scissor[0] + scissor[2], int32_t(std::floor(320 * (1 + x))));
            scissor[0] = left; scissor[2] = std::max(0, right - left);
            state.scissor = scissor;
        }
    }
    // The button prompts (pictures over the SDK quads of the text): the same narrowing as the text line they stand in.
    const auto& source = packet.display_source;
    if (source[2] && source[3]) for (auto& prompt : packet.prompts) {
        const float sx = 2.f / source[2], sy = 2.f / source[3];
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (size_t i = 0; i < 4; ++i) {
            const float x = (prompt.efb_corners[i * 2] - source[0]) * sx - 1.f, y = (prompt.efb_corners[i * 2 + 1] - source[1]) * sy - 1.f;
            x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
        }
        if (!std::isfinite(x0 + x1 + y0 + y1)) continue;
        const Anchor anchor = prompt_anchor(frame, x0, x1, y0, y1);
        const auto move_x = [&](float efb) { const float x = (efb - source[0]) * sx - 1.f; return ((k.x * x + anchor.tx - k.x * anchor.cx) + 1.f) / sx + source[0]; };
        const auto move_y = [&](float efb) { const float y = (efb - source[1]) * sy - 1.f; return ((k.y * y + anchor.ty - k.y * anchor.cy) + 1.f) / sy + source[1]; };
        for (size_t i = 0; i < 4; ++i) { prompt.efb_corners[i * 2] = move_x(prompt.efb_corners[i * 2]); prompt.efb_corners[i * 2 + 1] = move_y(prompt.efb_corners[i * 2 + 1]); }
        prompt.clip = {move_x(prompt.clip[0]), move_y(prompt.clip[1]), move_x(prompt.clip[2]), move_y(prompt.clip[3])};   // the rectangle the text is clipped to moves with it
    }
    if (packet.present) { set_view_scene(frame.scene); frame.scene = false; frame.boxes.clear(); frame.players.clear(); }
}
}
