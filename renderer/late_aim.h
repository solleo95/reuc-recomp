#pragma once
#include "gx_vertices.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>
#include <vector>
#include <optional>

namespace gx {
// The aim reticle is drawn by the game from the pointer it read at the start of its logic step, so it trails the mouse by up to a step plus the frames
// in flight. The renderer re-aims it at record time: the draws below are shifted by (freshest pointer - pointer the game drew with), which also keeps
// whatever the game adds itself (recoil, easing). Aim units: [-1,1] spans 1.25 screen widths and heights x 1.67 (aim = ndc * (0.8, 0.6)).
constexpr float kAimToNdcX = 1.f / 0.8f, kAimToNdcY = 1.f / 0.6f;
// Quads of the reticle ring and centre dot: (callsite, square texture edge). Found with the harness `draws` request at three aim points.
inline bool reticle_callsite(uint32_t callsite) { return callsite == 0x80393B0C || callsite == 0x8006D174; }
inline bool reticle_texture(const TexturePixels& texture) {
    if (texture.job && !texture.job->done.load(std::memory_order_acquire)) return false;   // not decoded yet: decide on a later frame
    if (texture.levels.empty()) return false;
    const auto& level = texture.levels[0];
    const uint32_t width = texture.guest_width ? texture.guest_width : level.width, height = texture.guest_height ? texture.guest_height : level.height;
    return width == height && (width == 100 || width == 120 || width == 154 || width == 178 || width == 200);   // 178: the red halo of the empty-magazine / reload state; 200: the four red arrows of the held A+B / grab prompt
}
// Centre of the draw's screen-space bounding box in Vulkan NDC (x right, y down, like aim units); the harness's draw summary negates y.
inline std::optional<std::array<float,2>> draw_center_ndc(const DrawPacket& draw) {
    const auto& t = draw.transforms;
    const bool indexed = draw.state->descriptors[0] != Mode::none;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; bool any = false;
    for (const auto& vertex : draw.vertex_view()) {
        const uint32_t row = indexed ? vertex.position_matrix : t->current;
        if (row > 61) continue;
        const auto& p = vertex.position; float view[3];
        for (int i = 0; i < 3; ++i) { const auto& m = t->position_rows[row + i]; view[i] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]; }
        float clip[4];
        for (int i = 0; i < 4; ++i) { const auto& m = t->projection[i]; clip[i] = m[0] * view[0] + m[1] * view[1] + m[2] * view[2] + m[3]; }
        if (clip[3] <= 0) continue;
        const float x = clip[0] / clip[3], y = clip[1] / clip[3];
        x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); any = true;
    }
    if (!any) return std::nullopt;
    return std::array<float,2>{(x0 + x1) / 2, (y0 + y1) / 2};
}
// The wake the game draws behind each reticle (a strip of its recent positions, in the player's tint): it follows the reticle when that is shifted.
inline bool reticle_tail_callsite(uint32_t callsite) { return callsite == 0x8006CED4; }
// NDC of an object-space point of this draw (the same chain as draw_center_ndc); nullopt behind the camera or with an unloaded row.
inline std::optional<std::array<float,2>> project_ndc(const DrawPacket& draw, uint32_t row, const std::array<float,3>& p) {
    const auto& t = draw.transforms;
    if (row > 61) return std::nullopt;
    float view[3];
    for (int i = 0; i < 3; ++i) { const auto& m = t->position_rows[row + i]; view[i] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]; }
    float clip[4];
    for (int i = 0; i < 4; ++i) { const auto& m = t->projection[i]; clip[i] = m[0] * view[0] + m[1] * view[1] + m[2] * view[2] + m[3]; }
    if (clip[3] <= 0) return std::nullopt;
    return std::array<float,2>{clip[0] / clip[3], clip[1] / clip[3]};
}
// The new NDC positions of a wake strip (vertex pairs L,R, newest pair first) laid along `path` (NDC at an age in seconds, 0 = now) instead of along the
// path the game's own simulation took: the pair k sits `duration * k / (pairs - 1)` seconds back, keeps its half-width and its side, and turns with the
// path. Used for the local player's wake, whose reticle follows the mouse itself while the strip describes the lagging simulated one.
template<class Path> std::vector<std::array<float,2>> wake_strip_ndc(const std::vector<std::array<float,2>>& ndc, double duration, const Path& path) {
    const size_t pairs = ndc.size() / 2;
    if (pairs < 3 || ndc.size() % 2) return ndc;
    std::vector<std::array<float,2>> center(pairs);
    for (size_t k = 0; k < pairs; ++k) { center[k] = path(duration * double(k) / double(pairs - 1)); }
    std::vector<std::array<float,2>> out(ndc.size());
    for (size_t k = 0; k < pairs; ++k) {
        const size_t newer = k ? k - 1 : 0, older = k + 1 < pairs ? k + 1 : k;
        const std::array<float,2> half{(ndc[2*k+1][0] - ndc[2*k][0]) / 2, (ndc[2*k+1][1] - ndc[2*k][1]) / 2};
        const auto mid = [&](size_t i) { return std::array<float,2>{(ndc[2*i][0] + ndc[2*i+1][0]) / 2, (ndc[2*i][1] + ndc[2*i+1][1]) / 2}; };
        const float ot_x = mid(newer)[0] - mid(older)[0], ot_y = mid(newer)[1] - mid(older)[1];   // the old path's direction (older -> newer)
        const float nt_x = center[newer][0] - center[older][0], nt_y = center[newer][1] - center[older][1];
        const float old_len = std::hypot(ot_x, ot_y), new_len = std::hypot(nt_x, nt_y), width = std::hypot(half[0], half[1]);
        std::array<float,2> h = half;
        if (old_len > 1e-6f && new_len > 1e-6f && width > 0) {
            const float side = (ot_x * half[1] - ot_y * half[0]) >= 0 ? 1.f : -1.f;   // which side of the path this vertex pair's R lies on
            h = {-side * width * nt_y / new_len, side * width * nt_x / new_len};
        }
        out[2*k] = {center[k][0] - h[0], center[k][1] - h[1]};
        out[2*k+1] = {center[k][0] + h[0], center[k][1] + h[1]};
    }
    return out;
}
// Lays the wake draw's vertices (GPU copies in `target`) along `path_aim` (aim units at an age in seconds). False where the draw's chain is not invertible.
template<class Path> bool retarget_wake(const DrawPacket& draw, GpuMeshVertex* target, size_t count, double duration, const Path& path_aim) {
    const auto source = draw.vertex_view();
    if (count != source.size() || count < 6 || count % 2) return false;
    const uint32_t row = draw.state->descriptors[0] != Mode::none ? source[0].position_matrix : draw.transforms->current;
    const std::array<float,3> p0 = source[0].position;
    const auto f0 = project_ndc(draw, row, p0), fx = project_ndc(draw, row, {p0[0] + 1, p0[1], p0[2]}), fy = project_ndc(draw, row, {p0[0], p0[1] + 1, p0[2]});
    if (!f0 || !fx || !fy) return false;
    const float a = (*fx)[0] - (*f0)[0], c = (*fx)[1] - (*f0)[1], b = (*fy)[0] - (*f0)[0], d = (*fy)[1] - (*f0)[1];   // ndc = f0 + [a b; c d] * (obj - p0)
    const float det = a * d - b * c;
    if (std::abs(det) < 1e-12f) return false;
    std::vector<std::array<float,2>> ndc(count);
    for (size_t i = 0; i < count; ++i) { const auto n = project_ndc(draw, row, source[i].position); if (!n) return false; ndc[i] = *n; }
    const auto moved = wake_strip_ndc(ndc, duration, [&](double age) { const auto aim = path_aim(age); return std::array<float,2>{aim[0] * kAimToNdcX, aim[1] * kAimToNdcY}; });
    for (size_t i = 0; i < count; ++i) {
        const float dx = moved[i][0] - (*f0)[0], dy = moved[i][1] - (*f0)[1];
        target[i].position[0] = p0[0] + (d * dx - b * dy) / det;
        target[i].position[1] = p0[1] + (-c * dx + a * dy) / det;
    }
    return true;
}
// A reticle quad candidate: the callsite and a decoded square texture of a known edge. Where it sits is judged by mark_reticle_draws.
inline bool reticle_candidate(const DrawPacket& draw) { return reticle_callsite(draw.callsite) && draw.texture && reticle_texture(*draw.texture); }
// The ring's vertex colour is the player's: red for the first, blue for the second (the harness saw 255,0,0 / 100,0,0 / 255,64,64 and 0,0,255 / 0,0,100 / 50,100,255).
// The white companion quads at the same place carry none. -1: no tint.
inline int reticle_tint_owner(const DrawPacket& draw) {
    const auto vertices = draw.vertex_view();
    if (vertices.empty()) return -1;
    const auto& c = vertices[0].color[0];
    if (std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}) < 40) return -1;
    return c[0] >= c[2] ? 0 : 1;
}
// Which player each reticle belongs to and which quads are reticles at all. The pointer a quad was drawn from is not a reliable key: the game eases the
// reticle behind its pointer, so a fast flick moves it 0.2 and more away from the consumed pointer and a window test lets it go and come back (the
// reticle jumped). The quads of one reticle sit at one place (ring in the player's tint, white companions); the first tinted quad names the owner.
// consumed[k]: the pointer pad k's last sample drew with (aim units), empty where the pad does not exist (single-player has only pad 0).
// Flags every reticle quad (`reticle`: the local crosshair size applies to both players' reticles) and the local player's (`late_aim`: it follows the mouse).
// exact: the local reticle is re-aimed to the mouse itself (aim_ref = the tinted ring's own centre, so every quad of that player moves by the same amount and
// keeps its offsets) instead of by (mouse - consumed pointer): the game's easing and recoil behind the pointer change at the tick rate and showed as jitter.
template<class Draws> size_t mark_reticle_draws(Draws& draws, const std::array<std::optional<std::array<float,2>>,2>& consumed, unsigned local, bool exact = false) {
    struct Group { std::array<float,2> center; int owner; std::vector<size_t> members; bool tinted = false; };
    std::vector<Group> groups;
    std::vector<std::pair<size_t, std::array<float,2>>> loose;   // untinted quads that did not directly follow a ring
    const auto within = [](const std::array<float,2>& a, const std::array<float,2>& b, float limit) { return std::abs(a[0] - b[0]) < limit && std::abs(a[1] - b[1]) < limit; };
    size_t last = size_t(-1);   // the group of the latest tinted ring: the white quads that follow it in draw order are its own, even where two reticles overlap
    for (size_t i = 0; i < draws.size(); ++i) {
        if (!reticle_candidate(draws[i])) continue;
        const auto center = draw_center_ndc(draws[i]);
        if (!center) continue;
        int tint = reticle_tint_owner(draws[i]);
        if (tint == 1 && !consumed[1]) tint = 0;   // a lone pad's reticle is the first player's, whatever its tint
        if (tint >= 0) {
            size_t found = groups.size();
            for (size_t g = 0; g < groups.size(); ++g) if (groups[g].tinted && groups[g].owner == tint && within(groups[g].center, *center, 0.03f)) { found = g; break; }
            if (found == groups.size()) groups.push_back({*center, tint, {}, true});
            groups[found].members.push_back(i);
            last = found;
        } else if (last != size_t(-1) && within(groups[last].center, *center, 0.03f)) groups[last].members.push_back(i);
        else loose.push_back({i, *center});
    }
    for (const auto& [index, center] : loose) {
        size_t best = groups.size(); float best_distance = 0.15f;
        for (size_t g = 0; g < groups.size(); ++g) {
            if (!groups[g].tinted) continue;
            const float d = std::max(std::abs(groups[g].center[0] - center[0]), std::abs(groups[g].center[1] - center[1]));
            if (d < best_distance) { best_distance = d; best = g; }
        }
        if (best == groups.size())
            for (size_t g = 0; g < groups.size(); ++g) if (!groups[g].tinted && within(groups[g].center, center, 0.03f)) { best = g; break; }
        if (best == groups.size()) groups.push_back({center, -1, {}, false});
        groups[best].members.push_back(index);
    }
    size_t marked = 0;
    std::optional<std::array<float,2>> anchor;   // the local ring's centre in aim units
    for (auto& group : groups)
        if (exact && group.tinted && group.owner >= 0 && unsigned(group.owner) == local && consumed[group.owner]) { anchor = std::array<float,2>{group.center[0] / kAimToNdcX, group.center[1] / kAimToNdcY}; break; }
    for (auto& group : groups) {
        int owner = group.owner;
        if (owner < 0) {   // no tinted quad: the pointer nearest to the group decides, within the old window
            float best = 0.25f;
            for (int pad = 0; pad < 2; ++pad) {
                if (!consumed[pad]) continue;
                const float d = std::max(std::abs(group.center[0] - (*consumed[pad])[0] * kAimToNdcX), std::abs(group.center[1] - (*consumed[pad])[1] * kAimToNdcY));
                if (d < best) { best = d; owner = pad; }
            }
        }
        if (owner < 0 || !consumed[owner]) continue;
        // A ring far from its player's pointer is something else (the HUD draws other square textures through the same callsite).
        if (std::abs(group.center[0] - (*consumed[owner])[0] * kAimToNdcX) > 0.6f || std::abs(group.center[1] - (*consumed[owner])[1] * kAimToNdcY) > 0.6f) continue;
        for (const auto index : group.members) {
            draws[index].reticle = draws[index].layer = true; draws[index].late_aim = unsigned(owner) == local; ++marked;
            if (draws[index].late_aim && anchor) { draws[index].aim_ref = *anchor; draws[index].aim_exact = true; }
        }
    }
    for (size_t i = 0; i < draws.size(); ++i) {   // the wake of the local reticle moves with it (not scaled: it has no centre of its own)
        if (!reticle_tail_callsite(draws[i].callsite)) continue;
        draws[i].layer = true;   // every player's wake belongs to the reticle layer, only the local one is re-aimed
        int owner = reticle_tint_owner(draws[i]);
        if (owner == 1 && !consumed[1]) owner = 0;
        if (exact && owner >= 0 && unsigned(owner) == local && consumed[owner]) {   // single-player: the wake stays as the game draws it (a rigid shift swung it and smeared under frame generation)
            draws[i].late_aim = true; draws[i].wake = exact; ++marked;
            if (anchor) { draws[i].aim_ref = *anchor; draws[i].aim_exact = true; }
        }
    }
    return marked;
}
// Crosshair scale (graphics.crosshair_scale): the draw's viewport is scaled about the reticle centre (draw NDC), so the quad keeps its place and changes size.
inline void scale_viewport_about(const std::array<float,2>& center, float scale, float& x, float& y, float& width, float& height) {
    x += (center[0] * 0.5f + 0.5f) * width * (1 - scale); width *= scale;
    y += (center[1] * 0.5f + 0.5f) * height * (1 - scale); height *= scale;
}
}
