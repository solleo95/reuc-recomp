#pragma once
#include <cstdint>
namespace uc {
enum class PointerMode:uint8_t { relative,absolute };   // the mouse as motion (default), or the window position of the cursor: lightguns, a Wii Remote bar in mouse mode, tablets
enum class ReloadZone:uint8_t { off,corner,edge };      // a shot fired there reloads instead (lightguns have no off-screen reload of their own)
// The game's pointer (KPAD aim, -1..1) spans 1.25 screen widths and 1.67 heights: the screen's edge is at .8 across and .6 down (renderer/late_aim.h: aim = ndc * (0.8, 0.6)).
// An absolute device reports a place in the window, so it is scaled by these to put the reticle under the cursor.
constexpr float kAimEdgeX=0.8f,kAimEdgeY=0.6f;
constexpr uint32_t kPointerSmoothingMax=90;            // percent of the old pointer place that stays at each new absolute position
constexpr uint32_t kReloadZoneSizeMin=1,kReloadZoneSizeMax=20;   // percent of the window's width and height
}
