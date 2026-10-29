#pragma once
#include <array>
#include <cstdint>
#include <mutex>
namespace gx {
// How this screen draws the aim reticle (the in-game menu's Reticle tab). Read by the guest thread for every packet (late_aim.h apply_reticle_style),
// written by the menu: hidden parts, the players' colours, the dot's size, the wake's length and the partner's opacity.
struct ReticleStyle {
    bool hide_tail = false, hide_dot = false, hide_marks = false, hide_arc = false;
    std::array<uint32_t,2> color{0, 0};   // 0xRRGGBB per player (0 = the first); 0 keeps the game's own red / blue
    float dot_scale = 1;                  // the laser dot's size
    float partner_alpha = 1;              // the other player's reticle, 0 = invisible
    bool operator==(const ReticleStyle&) const = default;
    // True when no draw needs touching (the common case costs one pass over the packet's draws at most).
    bool plain() const { return *this == ReticleStyle{}; }
};
inline std::mutex& reticle_style_mutex() { static std::mutex mutex; return mutex; }
inline ReticleStyle& reticle_style_storage() { static ReticleStyle style; return style; }
inline ReticleStyle reticle_style() { std::lock_guard lock(reticle_style_mutex()); return reticle_style_storage(); }
inline void set_reticle_style(const ReticleStyle& style) { std::lock_guard lock(reticle_style_mutex()); reticle_style_storage() = style; }
}
