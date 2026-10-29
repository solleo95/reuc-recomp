#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cstdlib>

namespace gx {
// Native ultrawide (see ultrawide.h): the game draws a 16:9 picture; on another shape of output it is asked for a wider (taller) one and its 2D
// elements are narrowed to keep their shape. These are the fractions of the output's width (x) and height (y) the game's 16:9 picture covers: both 1
// at 16:9, x < 1 on a wider output, y < 1 on a taller one. `edges`: on a wider output a player's block of the in-game interface keeps its margin from
// the screen edge instead of moving to the middle. Set from the output's size, read by game/intro.cpp (the projection) and the packet producer.
constexpr double kGameAspect = 16.0 / 9;
struct ViewFactors { float x = 1.f, y = 1.f; bool edges = false; };
inline ViewFactors factors_for_aspect(double aspect, bool edges = false) {
    if (aspect > kGameAspect * 1.001) return {float(kGameAspect / aspect), 1.f, edges};
    if (aspect < kGameAspect / 1.001 && aspect > 0) return {1.f, float(aspect / kGameAspect), false};
    return {};
}
// x and y travel in one atomic word, so a reader never sees the x of one window shape with the y of another; `edges` only moves the interface.
inline std::atomic<uint64_t>& view_state() { static std::atomic<uint64_t> state{uint64_t(0x3F800000u) | uint64_t(0x3F800000u) << 32}; return state; }   // 1.0f | 1.0f << 32
inline std::atomic<bool>& view_edges_state() { static std::atomic<bool> edges{false}; return edges; }
inline void set_view_factors(ViewFactors factors) {
    uint32_t x, y; std::memcpy(&x, &factors.x, 4); std::memcpy(&y, &factors.y, 4);
    view_state().store(uint64_t(x) | uint64_t(y) << 32); view_edges_state().store(factors.edges);
}
inline ViewFactors view_factors() {
    const uint64_t state = view_state().load();
    const uint32_t x = uint32_t(state), y = uint32_t(state >> 32);
    ViewFactors factors; std::memcpy(&factors.x, &x, 4); std::memcpy(&factors.y, &y, 4); factors.edges = view_edges_state().load();
    return factors;
}
// Whether the last presented frame had a 3D scene. Without one (a menu, a title) the 2D layout is narrowed to k of the output and the pointer the game reads is
// scaled by 1 / k to match (runtime/native_input.cpp); with one the game's own projection is wide and the pointer is left alone.
inline std::atomic<bool>& view_scene_state() { static std::atomic<bool> scene{false}; return scene; }
inline void set_view_scene(bool scene) { view_scene_state().store(scene); }
inline bool view_scene() { return view_scene_state().load(); }
inline float view_factor_x() { return view_factors().x; }
inline float view_factor_y() { return view_factors().y; }
// Where an orthographic draw of a frame with a 3D scene is narrowed: about (cx, cy) in NDC, the point landing on (tx, ty). Anchored: false leaves the draw alone.
struct Anchor { float cx = 0, cy = 0, tx = 0, ty = 0; bool anchored = false; };
// A narrowed draw's rectangle (NDC, before the narrowing) and its anchor: what a later packet of the same frame (the button prompts, which come with the
// presenting packet) finds to take the same anchor as the text line they stand in.
struct AnchoredBox { float x0, x1, y0, y1; Anchor anchor; };
// A player's block, found by its health bar: the draws whose centre lies in x from..to and y y0..y1 belong to it, whatever packet they come in.
struct PlayerRegion { float from, to, y0, y1; Anchor anchor; };
// The producer's memory of the frame being built (touched by one thread at a time: the guest baton).
struct WideFrame {
    bool scene = false;                // the frame already had a perspective draw: the interface comes in a later packet
    std::vector<AnchoredBox> boxes;    // the anchored draws of the frame so far
    std::vector<PlayerRegion> players; // the player blocks of the frame so far
};

}
